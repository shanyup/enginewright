"""ewtool.py: build, run and check an Unreal project from outside the editor (Python 3 standard library only).

Part of Enginewright. The MCP server lives inside the editor; these jobs need the editor closed or run
the game itself, so they are a command line tool. Every command prints one JSON object (exit code 0 = ok).

Commands
  build    Compile a target with UnrealBuildTool. Waits for another build (UBT mutex) and for locked
           binaries, names the process that holds a locked DLL, and runs UBT in a detached worker: when the
           wait time is over UBT keeps running (never killed half way) and `status` follows it.
  status   State of a build started by `build` (running / succeeded / failed, errors, log).
  locks    Which processes hold the project's binaries (DLL/EXE) open.
  run      Start the game (`UnrealEditor.exe <project> <url> -game`, or a packaged exe) once per resolution,
           pass the URL unchanged (checked against the game's own log), wait for exit or time-out, keep the
           log lines you ask for, decide PASSED/FAILED from your patterns, and collect the new PNG files.
  test     `run` with a build first and a required verdict: compile -> run self-test -> PASSED/FAILED lines.
  compose  Put 2-4 (or more) PNG images side by side in one labelled PNG (rows x columns).

Placeholders in --url and --arg: {shots} (per-run output folder, forward slashes), {out}, {w}, {h}, {res}.

Examples
  python ewtool.py build  --project D:/P/MyGame.uproject
  python ewtool.py run    --project D:/P/MyGame.uproject --url "/Game/Maps/Menu?Shots={shots}?Quit=1" \
                          --res 1920x1080 --res 1280x720 --filter "TEST " --pass "ALL PASSED" --fail "FAILED"
  python ewtool.py test   --project D:/P/MyGame.uproject --url "/Game/Maps/Test?SelfTest=Logic" \
                          --filter "TEST " --filter "[Save]" --pass "TEST .*PASSED" --fail "TEST .*FAILED"
  python ewtool.py compose --out cmp.png --cols "Web,Game" --row 1920 web1920.png game1920.png \
                          --row 1280 web1280.png game1280.png --title "Main menu"
"""
import argparse, datetime, json, os, pathlib, re, shutil, subprocess, sys, time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ew_png  # noqa: E402

IS_WIN = os.name == "nt"
CRASH_RE = re.compile(r"Fatal error|Unhandled Exception|=== Critical error ===|Assertion failed|appError called")


class Fail(Exception):
    """A user-facing error: printed as {"ok": false, "error": code, "message": ...}."""

    def __init__(self, code, message, **extra):
        super().__init__(message)
        self.code, self.message, self.extra = code, message, extra


def emit(obj, code=None):
    print(json.dumps(obj, indent=1, ensure_ascii=False, default=str))
    sys.stdout.flush()
    sys.exit(code if code is not None else (0 if obj.get("ok") else 1))


def fwd(p):
    return str(p).replace("\\", "/")


def now_tag():
    return datetime.datetime.now().strftime("%Y%m%d-%H%M%S")


# ---------------------------------------------------------------- project and engine

def load_project(path):
    p = pathlib.Path(path).resolve()
    if p.is_dir():
        found = sorted(p.glob("*.uproject"))
        if len(found) != 1:
            raise Fail("NO_PROJECT", f"expected one .uproject in {p}, found {len(found)}")
        p = found[0]
    if p.suffix.lower() != ".uproject" or not p.is_file():
        raise Fail("NO_PROJECT", f"not a .uproject file: {p}")
    try:
        meta = json.loads(p.read_text(encoding="utf-8-sig"))
    except ValueError as e:
        raise Fail("NO_PROJECT", f"cannot parse {p}: {e}")
    return p, meta


def find_engine(meta, given):
    if given:
        e = pathlib.Path(given)
        if not (e / "Engine" / "Build" / "Build.version").is_file():
            raise Fail("NO_ENGINE", f"not an engine folder (no Engine/Build/Build.version): {e}")
        return e
    if os.environ.get("UE_ENGINE_DIR"):
        return find_engine(meta, os.environ["UE_ENGINE_DIR"])
    assoc = meta.get("EngineAssociation", "")
    cands = []
    if IS_WIN:
        import winreg
        for root, key in ((winreg.HKEY_LOCAL_MACHINE, rf"SOFTWARE\EpicGames\Unreal Engine\{assoc}"),):
            try:
                with winreg.OpenKey(root, key) as k:
                    cands.append(winreg.QueryValueEx(k, "InstalledDirectory")[0])
            except OSError:
                pass
        try:  # source builds registered by GUID
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Epic Games\Unreal Engine\Builds") as k:
                cands.append(winreg.QueryValueEx(k, assoc)[0])
        except OSError:
            pass
        dat = pathlib.Path(os.environ.get("PROGRAMDATA", "C:/ProgramData")) / "Epic/UnrealEngineLauncher/LauncherInstalled.dat"
        if dat.is_file():
            try:
                for it in json.loads(dat.read_text(encoding="utf-8")).get("InstallationList", []):
                    if it.get("AppName") == f"UE_{assoc}":
                        cands.append(it.get("InstallLocation"))
            except ValueError:
                pass
    for c in cands:
        if c and (pathlib.Path(c) / "Engine" / "Build" / "Build.version").is_file():
            return pathlib.Path(c)
    raise Fail("NO_ENGINE", f"engine '{assoc}' not found; pass --engine <engine root> or set UE_ENGINE_DIR")


def default_target(proj):
    src = proj.parent / "Source"
    name = proj.stem
    if (src / f"{name}Editor.Target.cs").is_file():
        return f"{name}Editor"
    targets = sorted(src.glob("*Editor.Target.cs")) if src.is_dir() else []
    if targets:
        return targets[0].name[:-len(".Target.cs")]
    raise Fail("NO_TARGET", f"{proj.name} has no C++ editor target in Source/ (Blueprint-only projects need no build)")


def runs_dir(proj):
    d = proj.parent / "Saved" / "Enginewright" / "runs"
    d.mkdir(parents=True, exist_ok=True)
    return d


# ---------------------------------------------------------------- processes and locks

def processes(names_re=r"UnrealEditor|UnrealBuildTool|dotnet|UnrealGame|ShaderCompileWorker"):
    """[{pid, name, commandLine}] for processes whose name matches (Windows: CIM; elsewhere: ps)."""
    if IS_WIN:
        ps = ("Get-CimInstance Win32_Process | Where-Object { $_.Name -match '%s' } | "
              "Select-Object ProcessId,Name,CommandLine,CreationDate | ConvertTo-Json -Compress" % names_re)
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                                 capture_output=True, text=True, timeout=60).stdout.strip()
        except Exception:
            return []
        if not out:
            return []
        rows = json.loads(out)
        rows = rows if isinstance(rows, list) else [rows]
        def when(v):
            m = re.search(r"Date\((\d+)", str(v or ""))
            return datetime.datetime.fromtimestamp(int(m.group(1)) / 1000).isoformat(timespec="seconds") if m else v
        return [{"pid": r["ProcessId"], "name": r["Name"], "commandLine": r.get("CommandLine") or "",
                 "started": when(r.get("CreationDate"))} for r in rows]
    out = subprocess.run(["ps", "-eo", "pid=,comm=,args="], capture_output=True, text=True).stdout
    res = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) >= 2 and re.search(names_re, parts[1]):
            res.append({"pid": int(parts[0]), "name": parts[1], "commandLine": parts[2] if len(parts) > 2 else ""})
    return res


def ubt_processes():
    return [p for p in processes() if "unrealbuildtool" in (p["name"] + p["commandLine"]).lower()]


def lock_pids(files):
    """{pid: app name} of processes that have any of the files open (Windows Restart Manager)."""
    files = [str(f) for f in files if pathlib.Path(f).is_file()]
    if not IS_WIN or not files:
        return {}
    import ctypes
    from ctypes import wintypes as wt

    class UNIQUE(ctypes.Structure):
        _fields_ = [("dwProcessId", wt.DWORD), ("ProcessStartTime", wt.FILETIME)]

    class INFO(ctypes.Structure):
        _fields_ = [("Process", UNIQUE), ("strAppName", wt.WCHAR * 256), ("strServiceShortName", wt.WCHAR * 64),
                    ("ApplicationType", ctypes.c_int), ("AppStatus", wt.ULONG), ("TSSessionId", wt.DWORD),
                    ("bRestartable", wt.BOOL)]

    rm = ctypes.WinDLL("rstrtmgr")
    sess = wt.DWORD()
    key = ctypes.create_unicode_buffer(33)
    if rm.RmStartSession(ctypes.byref(sess), 0, key):
        return {}
    try:
        arr = (wt.LPCWSTR * len(files))(*files)
        if rm.RmRegisterResources(sess, len(files), arr, 0, None, 0, None):
            return {}
        need, have, reasons = wt.UINT(0), wt.UINT(0), wt.DWORD()
        rc = rm.RmGetList(sess, ctypes.byref(need), ctypes.byref(have), None, ctypes.byref(reasons))
        if rc not in (0, 234) or need.value == 0:
            return {}
        infos = (INFO * need.value)()
        have = wt.UINT(need.value)
        if rm.RmGetList(sess, ctypes.byref(need), ctypes.byref(have), infos, ctypes.byref(reasons)):
            return {}
        return {i.Process.dwProcessId: i.strAppName for i in infos[:have.value]}
    finally:
        rm.RmEndSession(sess)


def lock_holders(files, per_file=False):
    """[{pid, name, commandLine, started[, files]}] of processes holding any of the files open. [] when none."""
    pids = lock_pids(files)
    details = {p["pid"]: p for p in processes(".")} if pids else {}
    res = []
    for pid, app in pids.items():
        d = details.get(pid, {})
        res.append({"pid": pid, "name": d.get("name") or app, "commandLine": d.get("commandLine", ""),
                    "started": d.get("started")})
    if per_file and res:
        owners = {fwd(f): lock_pids([f]) for f in files}
        for r in res:
            r["files"] = [f for f, ps in owners.items() if r["pid"] in ps][:20]
    return res


def project_binaries(proj):
    roots = [proj.parent / "Binaries"]
    plugins = proj.parent / "Plugins"
    if plugins.is_dir():
        for up in plugins.rglob("*.uplugin"):
            roots.append(up.parent / "Binaries")
    files = []
    for r in roots:
        if r.is_dir():
            files += [f for f in r.rglob("*") if f.suffix.lower() in (".dll", ".exe")]
    return files


# ---------------------------------------------------------------- build

def build_state_path(proj, bid):
    return runs_dir(proj) / f"build-{bid}.json"


def read_state(path):
    for _ in range(20):
        try:
            return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
        except (ValueError, OSError):
            time.sleep(0.1)
    raise Fail("BAD_STATE", f"cannot read build state {path}")


def write_state(path, st):
    tmp = str(path) + ".tmp"
    pathlib.Path(tmp).write_text(json.dumps(st, indent=1), encoding="utf-8")
    os.replace(tmp, path)


def pid_alive(pid):
    if not pid:
        return False
    if IS_WIN:
        import ctypes
        h = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(h)
        return code.value == 259  # STILL_ACTIVE
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def worker_main(state_path):
    """Detached worker: runs the build command, records child pid and exit code in the state file."""
    st = read_state(state_path)
    with open(st["log"], "w", encoding="utf-8", errors="replace") as log:
        p = subprocess.Popen(st["cmd"], stdout=log, stderr=subprocess.STDOUT)
        st.update(childPid=p.pid, workerPid=os.getpid())
        write_state(state_path, st)
        code = p.wait()
    st = read_state(state_path)
    st.update(exitCode=code, finished=time.time())
    write_state(state_path, st)


def spawn_detached(args):
    kw = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL, "close_fds": True}
    if IS_WIN:
        base = 0x00000008 | 0x00000200 | 0x08000000  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW
        try:  # leave the caller's job object, so a killed caller does not take UBT with it
            return subprocess.Popen(args, creationflags=base | 0x01000000, **kw), True
        except OSError:
            return subprocess.Popen(args, creationflags=base, **kw), False
    return subprocess.Popen(args, start_new_session=True, **kw), True


ERR_RE = re.compile(r"\): (fatal )?error |error (C|LNK|MSB)\d+|: Error: |ERROR: ")


def parse_build_log(text):
    lines = text.splitlines()
    errors, seen = [], set()
    for l in lines:
        if ERR_RE.search(l):
            k = l.strip()
            if k not in seen:
                seen.add(k)
                errors.append(k[:400])
    result = next((l.split("Result:", 1)[1].strip() for l in reversed(lines) if "Result:" in l), None)
    actions = [m.group(1) for m in (re.match(r"\[(\d+/\d+)\]", l.strip()) for l in lines) if m]
    locked = []
    for l in lines:
        m = re.search(r"(?:LNK1104|cannot open file|Unable to delete|being used by another process)[^']*'([^']+\.(?:dll|exe|pdb))'", l, re.I)
        if m and m.group(1) not in locked:
            locked.append(m.group(1))
    waiting = any(re.search(r"mutex|another instance|ConflictingInstance", l, re.I) for l in lines[-30:])
    return {"result": result, "upToDate": "Target is up to date" in text, "errors": errors[:40],
            "errorCount": len(errors), "actions": actions[-1] if actions else None,
            "lockedFiles": locked, "waitingForOtherBuild": waiting, "tail": [l[:300] for l in lines[-8:]]}


def build_report(proj, st_path):
    st = read_state(st_path)
    text = pathlib.Path(st["log"]).read_text(encoding="utf-8", errors="replace") if pathlib.Path(st["log"]).is_file() else ""
    info = parse_build_log(text)
    running = st.get("exitCode") is None and (pid_alive(st.get("childPid")) or pid_alive(st.get("workerPid")) or
                                             (not st.get("childPid") and time.time() - st["started"] < 30))
    if running:
        status = "running"
    elif st.get("exitCode") is None:
        status = "lost"  # worker died without recording an exit code
    else:
        status = "succeeded" if st["exitCode"] == 0 else "failed"
    rep = {"ok": status == "succeeded", "status": status, "id": st["id"], "target": st["target"], "exitCode": st.get("exitCode"),
           "seconds": round((st.get("finished") or time.time()) - st["started"]), "log": fwd(st["log"]),
           "detached": st.get("detached"), **info}
    if info["lockedFiles"]:
        rep["lockHolders"] = lock_holders(info["lockedFiles"])
    if status == "running":
        if info["waitingForOtherBuild"]:
            rep["otherBuilds"] = [p for p in ubt_processes() if p["pid"] != st.get("childPid")]
        rep["hint"] = f"still running; follow with: ewtool.py status --project \"{fwd(proj)}\" --id {st['id']} --wait 600"
    return rep


def cmd_build(a, proj=None, meta=None):
    if proj is None:
        proj, meta = load_project(a.project)
    engine = find_engine(meta, a.engine)
    target = a.target or default_target(proj)
    ubt = engine / "Engine" / "Binaries" / "DotNET" / "UnrealBuildTool" / ("UnrealBuildTool.exe" if IS_WIN else "UnrealBuildTool")
    if not ubt.is_file():
        raise Fail("NO_UBT", f"UnrealBuildTool not found: {ubt}")
    # Binaries in use by a running editor/game cannot be replaced: wait, then name who holds them.
    files = project_binaries(proj)
    deadline = time.time() + a.lock_wait
    locked = lock_pids(files)
    while locked and time.time() < deadline:
        time.sleep(5)
        locked = lock_pids(files)
    holders = lock_holders(files, per_file=True) if locked else []
    if holders:
        raise Fail("LOCKED", f"{len(holders)} process(es) hold the project's binaries open; close them (an open editor of this "
                             "project counts) and build again", holders=holders,
                   hint="a process that cannot be closed (zombie) keeps the DLL locked until it ends or the PC restarts")
    bid = now_tag() + f"-{os.getpid()}"
    log = runs_dir(proj) / f"build-{bid}.log"
    cmd = [str(ubt), target, a.platform, a.config, f"-Project={proj}", "-WaitMutex", "-NoHotReloadFromIDE"] + list(a.ubt_arg or [])
    st_path = build_state_path(proj, bid)
    write_state(st_path, {"id": bid, "target": target, "cmd": cmd, "log": str(log), "started": time.time(),
                          "exitCode": None, "project": str(proj)})
    _, detached = spawn_detached([sys.executable, str(HERE / "ewtool.py"), "_worker", str(st_path)])
    st = read_state(st_path)
    st["detached"] = detached
    write_state(st_path, st)
    if a.background:
        return build_report(proj, st_path)
    deadline = time.time() + a.wait
    while time.time() < deadline:
        rep = build_report(proj, st_path)
        if rep["status"] != "running":
            return rep
        time.sleep(2)
    return build_report(proj, st_path)


def cmd_status(a):
    proj, _ = load_project(a.project)
    files = sorted(runs_dir(proj).glob("build-*.json"))
    if not files:
        raise Fail("NO_BUILD", "no build started by ewtool.py in this project")
    path = build_state_path(proj, a.id) if a.id and a.id != "latest" else files[-1]
    if not path.is_file():
        raise Fail("NO_BUILD", f"no build with id {a.id}")
    deadline = time.time() + (a.wait or 0)
    rep = build_report(proj, path)
    while rep["status"] == "running" and time.time() < deadline:
        time.sleep(2)
        rep = build_report(proj, path)
    return rep


def cmd_locks(a):
    proj, _ = load_project(a.project)
    files = [pathlib.Path(f) for f in a.file] if a.file else project_binaries(proj)
    holders = lock_holders(files, per_file=True)
    procs = processes()
    return {"ok": True, "locked": bool(holders), "filesChecked": len(files), "holders": holders,
            "editors": [p for p in procs if p["name"].lower().startswith("unrealeditor") and proj.stem.lower() in p["commandLine"].lower()],
            "builds": [p for p in procs if "unrealbuildtool" in (p["name"] + p["commandLine"]).lower()]}


# ---------------------------------------------------------------- run / test

def parse_res(s):
    m = re.fullmatch(r"\s*(\d{2,5})\s*[xX*]\s*(\d{2,5})\s*", s)
    if not m:
        raise Fail("BAD_ARGS", f"resolution must look like 1920x1080, got '{s}'")
    return int(m.group(1)), int(m.group(2))


def fill_placeholders(s, values):
    def rep(m):
        k = m.group(1)
        if k not in values:
            raise Fail("BAD_ARGS", f"unknown placeholder {{{k}}} in '{s}' (known: {', '.join(sorted(values))})")
        return values[k]
    return re.sub(r"\{(\w+)\}", rep, s)


def read_log_text(path):
    try:
        data = pathlib.Path(path).read_bytes()
    except OSError:
        return ""
    if data[:2] in (b"\xff\xfe", b"\xfe\xff") or (len(data) > 3 and data[1:2] == b"\0" and data[3:4] == b"\0"):
        return data.decode("utf-16", errors="replace")
    return data.decode("utf-8-sig", errors="replace")


def kill_tree(pid):
    if IS_WIN:
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
    else:
        try:
            os.killpg(os.getpgid(pid), 9)  # type: ignore[attr-defined]  # POSIX only
        except OSError:
            pass


def collect_pngs(dirs, since, dest, res):
    found, seen = [], set()
    for d in dirs:
        d = pathlib.Path(d)
        if not d.is_dir():
            continue
        for f in sorted(d.rglob("*.png")):
            try:
                if f.stat().st_mtime < since - 1 or f.resolve() in seen:
                    continue
            except OSError:
                continue
            seen.add(f.resolve())
            src = f
            if dest not in f.resolve().parents:  # copy shots written outside the run folder into it
                dest.mkdir(parents=True, exist_ok=True)
                target = dest / f.name
                n = 1
                while target.exists():
                    target = dest / f"{f.stem}_{n}{f.suffix}"
                    n += 1
                shutil.copy2(f, target)
                f = target
            item = {"path": fwd(f), "bytes": f.stat().st_size}
            if src != f:
                item["copiedFrom"] = fwd(src)
            try:
                w, h = ew_png.png_size(str(f))
                item.update(width=w, height=h, matchesRes=(res is None or (w, h) == res))
            except ew_png.PngError as e:
                item["error"] = str(e)
            found.append(item)
    return found


def run_once(a, proj, engine, url_t, res, out_dir, extra_t):
    w, h = res if res else (None, None)
    tag = f"{w}x{h}" if res else "default"
    rdir = out_dir / tag
    shots = rdir / "shots"
    if not a.dry_run:
        shots.mkdir(parents=True, exist_ok=True)
    vals = {"shots": fwd(shots), "out": fwd(rdir), "w": str(w or ""), "h": str(h or ""), "res": tag}
    url = fill_placeholders(url_t, vals) if url_t else ""
    extra = [fill_placeholders(x, vals) for x in extra_t]
    log = rdir / "game.log"
    if a.exe:
        cmd = [a.exe] + list(a.exe_arg or []) + ([url] if url else [])
    else:
        cmd = [str(engine / "Engine" / "Binaries" / ("Win64" if IS_WIN else "Linux") / ("UnrealEditor.exe" if IS_WIN else "UnrealEditor")),
               str(proj)] + ([url] if url else []) + ["-game"]
    if res:
        cmd += [f"-ResX={w}", f"-ResY={h}", "-fullscreen" if a.fullscreen else "-windowed"]
    cmd += [f"-abslog={log}", "-log", "-nosplash"] + ([] if a.no_unattended else ["-unattended"]) + extra
    rep = {"res": tag, "command": cmd, "log": fwd(log), "url": url}
    if a.dry_run:
        return rep
    pass_re = [re.compile(x) for x in a.pass_] if a.pass_ else []
    fail_re = [re.compile(x) for x in a.fail] if a.fail else []
    done_re = re.compile(a.done) if a.done else None
    req_re = re.compile(a.require) if a.require else None
    t0 = time.time()
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    url_ok = None if url else True
    stop_reason, done_at, text = None, None, ""
    while True:
        exited = p.poll() is not None
        text = read_log_text(log)
        if url_ok is None:
            cl = next((l for l in text.splitlines() if "Command Line:" in l), None)
            if cl is not None:
                url_ok = url in cl
                rep["commandLineSeen"] = cl.split("Command Line:", 1)[1].strip()[:1000]
                if not url_ok:
                    stop_reason = "URL_MISMATCH"
            elif time.time() - t0 > a.url_check_timeout:
                stop_reason = "URL_NOT_CONFIRMED"
        if not stop_reason and req_re and time.time() - t0 > a.require_within and not req_re.search(text):
            stop_reason = "REQUIRED_LINE_MISSING"
        if not stop_reason and done_re and done_at is None and done_re.search(text):
            done_at = time.time()
        if not stop_reason and done_at is not None and time.time() - done_at > a.done_grace and not exited:
            stop_reason = "DONE_BUT_NOT_EXITED"
        if exited or stop_reason or time.time() - t0 > a.timeout:
            break
        time.sleep(0.5)
    timed_out = not exited and not stop_reason
    if url_ok is None and not stop_reason:
        stop_reason = "URL_NOT_CONFIRMED"  # the game ended (or was stopped) before logging its command line
    if not exited:
        kill_tree(p.pid)
        try:
            p.wait(30)
        except subprocess.TimeoutExpired:
            pass
        time.sleep(1)
        text = read_log_text(log)
    lines = text.splitlines()
    filters = a.filter or []
    keep = [l for l in lines if any(f in l for f in filters) or any(r.search(l) for r in pass_re + fail_re) or CRASH_RE.search(l)]
    fails = [l for l in lines if any(r.search(l) for r in fail_re)]
    passes = [l for l in lines if any(r.search(l) for r in pass_re)]
    crash = [l for l in lines if CRASH_RE.search(l)]
    if stop_reason in ("URL_MISMATCH", "URL_NOT_CONFIRMED", "REQUIRED_LINE_MISSING"):
        verdict = stop_reason
    elif fails:
        verdict = "FAILED"
    elif crash:
        verdict = "CRASHED"
    elif pass_re:
        verdict = "PASSED" if passes else ("TIMEOUT" if timed_out else "NO_VERDICT")
    else:
        verdict = "TIMEOUT" if timed_out else ("EXITED_OK" if p.returncode == 0 else "EXIT_CODE")
    dirs = [shots] + [pathlib.Path(fill_placeholders(d, vals)) for d in (a.shots or [])]
    if not a.exe:
        dirs.append(proj.parent / "Saved" / "Screenshots")
    rep.update({
        "verdict": verdict, "exitCode": p.returncode if exited else None, "exited": exited, "timedOut": timed_out,
        "stopReason": stop_reason, "seconds": round(time.time() - t0, 1), "urlConfirmed": url_ok,
        "lines": [l[:500] for l in keep[:a.max_lines]], "lineCount": len(keep), "failLines": [l[:500] for l in fails[:50]],
        "crashLines": [l[:500] for l in crash[:20]], "screenshots": collect_pngs(dirs, t0, shots, res)})
    if not lines:
        rep["warning"] = "the game wrote no log; check the command (wrong exe/project?)"
    return rep


def cmd_run(a, require_verdict=False):
    proj, meta = load_project(a.project)
    if require_verdict and not (a.pass_ or a.fail):
        raise Fail("BAD_ARGS", "test needs --pass and/or --fail patterns to decide the result")
    engine = None if (a.exe and not a.build) else find_engine(meta, a.engine)
    if a.exe and not pathlib.Path(a.exe).is_file():
        raise Fail("BAD_ARGS", f"--exe not found: {a.exe}")
    res_list = [parse_res(r) for r in a.res] if a.res else [None]
    out = pathlib.Path(a.out) if a.out else runs_dir(proj) / f"run-{now_tag()}"
    if not a.dry_run:
        out.mkdir(parents=True, exist_ok=True)
    result = {"ok": False, "project": fwd(proj), "out": fwd(out), "build": None, "runs": []}
    if a.build and not a.dry_run:
        b = cmd_build(a, proj, meta)
        result["build"] = b
        if b["status"] != "succeeded":
            result["error"] = "BUILD_" + b["status"].upper()
            result["message"] = "build did not succeed; the game was not started" + \
                (" (still running: follow it with status, then run again)" if b["status"] == "running" else "")
            return result
    for res in res_list:
        result["runs"].append(run_once(a, proj, engine, a.url, res, out, a.arg or []))
    if a.dry_run:
        result["ok"] = True
        return result
    else:
        good = {"PASSED", "EXITED_OK"}
        result["ok"] = all(r["verdict"] in good for r in result["runs"])
        result["verdicts"] = {r["res"]: r["verdict"] for r in result["runs"]}
    (out / "summary.json").write_text(json.dumps(result, indent=1, ensure_ascii=False, default=str), encoding="utf-8")
    result["summary"] = fwd(out / "summary.json")
    return result


# ---------------------------------------------------------------- compose

def cmd_compose(a):
    rows = []
    if a.row:
        for r in a.row:
            if len(r) < 2:
                raise Fail("BAD_ARGS", "--row needs a label and at least one image: --row LABEL a.png [b.png ...]")
            rows.append((r[0], r[1:]))
    if a.images:
        rows.append((None, a.images))
    if not rows:
        raise Fail("BAD_ARGS", "give images (compose a.png b.png) or rows (--row LABEL a.png b.png)")
    cols = [c.strip() for c in a.cols.split(",")] if a.cols else None
    for _, paths in rows:
        for p in paths:
            if not os.path.isfile(p):
                raise Fail("NOT_FOUND", f"image not found: {p}")
    out = a.out
    pathlib.Path(out).parent.mkdir(parents=True, exist_ok=True)
    try:
        info = ew_png.compose(rows, out, cols, a.title, a.cell_width)
    except ew_png.PngError as e:
        raise Fail("BAD_IMAGE", str(e))
    return {"ok": True, **info}


# ---------------------------------------------------------------- CLI

def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["_worker"]:
        return worker_main(argv[1])
    ap = argparse.ArgumentParser(prog="ewtool.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--project", required=True, help="the .uproject file (or its folder)")
        p.add_argument("--engine", help="engine root (default: from EngineAssociation, or UE_ENGINE_DIR)")

    def build_args(p):
        p.add_argument("--target", help="build target (default <Project>Editor)")
        p.add_argument("--platform", default="Win64" if IS_WIN else "Linux")
        p.add_argument("--config", default="Development")
        p.add_argument("--wait", type=int, default=3600, help="seconds to wait for the build; UBT keeps running after it")
        p.add_argument("--lock-wait", type=int, default=120, help="seconds to wait for locked binaries to be released")
        p.add_argument("--ubt-arg", action="append", help="extra UnrealBuildTool argument (repeatable), e.g. --ubt-arg=-ForceUnity")
        p.add_argument("--background", action="store_true", help="start and return at once (follow with status)")

    p = sub.add_parser("build", help="compile a target")
    common(p)
    build_args(p)
    p = sub.add_parser("status", help="state of a build")
    p.add_argument("--project", required=True)
    p.add_argument("--id", default="latest")
    p.add_argument("--wait", type=int, default=0, help="wait up to N seconds for it to finish")
    p = sub.add_parser("locks", help="who holds the project's binaries")
    p.add_argument("--project", required=True)
    p.add_argument("--file", action="append", help="check this file instead of the project binaries (repeatable)")

    for name in ("run", "test"):
        p = sub.add_parser(name, help="run the game" if name == "run" else "build, run a self-test, report PASSED/FAILED")
        common(p)
        build_args(p)
        p.add_argument("--url", default="", help="map + options passed unchanged, e.g. /Game/Maps/Test?Mode=Logic")
        p.add_argument("--res", action="append", help="resolution WxH (repeatable: one run each)")
        p.add_argument("--fullscreen", action="store_true", help="fullscreen instead of windowed")
        p.add_argument("--exe", help="packaged game exe instead of UnrealEditor.exe -game")
        p.add_argument("--exe-arg", action="append", help="argument placed right after --exe, before the URL (repeatable)")
        p.add_argument("--arg", action="append", help="extra game argument (repeatable), e.g. --arg=-nosound")
        p.add_argument("--timeout", type=int, default=600, help="seconds per run before the game is stopped")
        p.add_argument("--filter", action="append", help="keep log lines containing this text (repeatable)")
        p.add_argument("--pass", dest="pass_", action="append", help="regex: a matching line means PASSED")
        p.add_argument("--fail", action="append", help="regex: a matching line means FAILED")
        p.add_argument("--done", help="regex: the run is over when this line appears (stop the game after --done-grace s)")
        p.add_argument("--done-grace", type=int, default=20)
        p.add_argument("--require", help="regex that must appear within --require-within s, else stop (guards against "
                                         "starting the real game when the test mode did not engage)")
        p.add_argument("--require-within", type=int, default=120)
        p.add_argument("--url-check-timeout", type=int, default=120, help="seconds to wait for the game log's command line")
        p.add_argument("--shots", action="append", help="extra folder to collect new PNGs from (repeatable, placeholders ok)")
        p.add_argument("--out", help="output folder (default <project>/Saved/Enginewright/runs/run-<time>)")
        p.add_argument("--max-lines", type=int, default=300)
        p.add_argument("--no-unattended", action="store_true", help="do not pass -unattended")
        p.add_argument("--dry-run", action="store_true", help="print the commands only")
        if name == "run":
            p.add_argument("--build", action="store_true", help="build first (waits for locks and other builds)")
        else:
            p.add_argument("--no-build", dest="build", action="store_false", help="skip the build")
            p.set_defaults(build=True)

    p = sub.add_parser("compose", help="labelled side-by-side PNG")
    p.add_argument("images", nargs="*", help="images for a single row")
    p.add_argument("--row", nargs="+", action="append", metavar="LABEL_THEN_IMAGES", help="--row LABEL a.png b.png (repeatable)")
    p.add_argument("--cols", help="column labels, comma separated (e.g. 'Web,Game')")
    p.add_argument("--title")
    p.add_argument("--cell-width", type=int, default=960, help="scale images down to this width (0 = original size)")
    p.add_argument("--out", required=True)

    a = ap.parse_args(argv)
    try:
        if a.cmd == "build":
            rep = cmd_build(a)
            emit(rep, 0 if rep["ok"] else (3 if rep["status"] == "running" else 1))
        elif a.cmd == "status":
            rep = cmd_status(a)
            emit(rep, 0 if rep["ok"] else (3 if rep["status"] == "running" else 1))
        elif a.cmd == "locks":
            emit(cmd_locks(a), 0)
        elif a.cmd in ("run", "test"):
            emit(cmd_run(a, require_verdict=(a.cmd == "test")))
        elif a.cmd == "compose":
            emit(cmd_compose(a))
    except Fail as e:
        emit({"ok": False, "error": e.code, "message": e.message, **e.extra}, 2)


if __name__ == "__main__":
    main()
