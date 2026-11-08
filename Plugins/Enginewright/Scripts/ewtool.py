"""ewtool.py: compile an Unreal project while its editor is closed (Python 3 standard library only).

Part of Enginewright, and optional: the MCP server inside the editor needs no Python. This helper only does the
one job the editor cannot do for itself: build the editor target while the editor is closed (its DLLs are locked
while it runs). Running the game, self-tests and screenshots are MCP tools (game_run, image_compose).
Every command prints one JSON object (exit code 0 = ok).

Commands
  build    Compile a target with UnrealBuildTool. Waits for another build (UBT mutex) and for locked
           binaries, names the process that holds a locked DLL, and runs UBT in a detached worker that is
           outside the caller's process tree and job object: when the wait time is over, or the caller is
           killed (an agent's tool time limit), UBT keeps running and `status` follows it.
  status   State of a build started by `build` (running / succeeded / failed / lost, errors, log).
  locks    Which processes hold the project's binaries (DLL/EXE) open.

Examples
  python ewtool.py build  --project D:/P/MyGame.uproject
  python ewtool.py build  --project D:/P/MyGame.uproject --background
  python ewtool.py status --project D:/P/MyGame.uproject --wait 600
  python ewtool.py locks  --project D:/P/MyGame.uproject
"""
import argparse, datetime, json, os, pathlib, re, subprocess, sys, time

HERE = pathlib.Path(__file__).resolve().parent
IS_WIN = os.name == "nt"


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
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, rf"SOFTWARE\EpicGames\Unreal Engine\{assoc}") as k:
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
    tmp = str(path) + f".{os.getpid()}.tmp"
    pathlib.Path(tmp).write_text(json.dumps(st, indent=1), encoding="utf-8")
    for _ in range(50):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:  # the other process is reading it right now
            time.sleep(0.05)
    os.replace(tmp, path)


def update_state(path, **kv):
    st = read_state(path)
    st.update(kv)
    write_state(path, st)
    return st


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
        p = subprocess.Popen(st["cmd"], stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             creationflags=0x08000000 if IS_WIN else 0)  # CREATE_NO_WINDOW
        update_state(state_path, childPid=p.pid, workerPid=os.getpid())
        code = p.wait()
    update_state(state_path, exitCode=code, finished=time.time())


def in_job():
    """True when this process runs inside a Windows job object (an agent's tool, a CI step, an IDE terminal)."""
    if not IS_WIN:
        return False
    import ctypes
    from ctypes import wintypes as wt
    r = wt.BOOL()
    k = ctypes.windll.kernel32
    return bool(k.IsProcessInJob(k.GetCurrentProcess(), None, ctypes.byref(r)) and r.value)


def spawn_wmi(args, cwd):
    """Starts args through WMI (Win32_Process.Create). The new process is a child of the WMI service host:
    outside the caller's process tree and job object, so killing the caller (taskkill /T, job close) does not
    reach it. Returns the pid or raises OSError."""
    env = dict(os.environ, EWTOOL_CMDLINE=subprocess.list2cmdline(args), EWTOOL_CWD=str(cwd))
    ps = ("$si = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ShowWindow=[uint16]0}; "
          "$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments "
          "@{CommandLine=$env:EWTOOL_CMDLINE; CurrentDirectory=$env:EWTOOL_CWD; ProcessStartupInformation=$si}; "
          "Write-Output \"$($r.ReturnValue) $($r.ProcessId)\"")
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], capture_output=True, text=True,
                         env=env, timeout=60, creationflags=0x08000000).stdout.split()
    if len(out) != 2 or out[0] != "0":
        raise OSError(f"Win32_Process.Create failed: {' '.join(out) or 'no answer'}")
    return int(out[1])


def spawn_detached(args, cwd):
    """Starts the build worker so that it outlives the caller. Returns how: wmi / breakaway / new-session / none."""
    if os.environ.get("EWTOOL_SPAWN") != "plain":
        if IS_WIN:
            try:
                spawn_wmi(args, cwd)
                return "wmi"
            except (OSError, subprocess.SubprocessError):
                pass
        else:
            subprocess.Popen(args, cwd=cwd, start_new_session=True, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
            return "new-session"
    kw = {"cwd": cwd, "stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL, "close_fds": True}
    base = 0x00000008 | 0x00000200 | 0x08000000  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW
    try:  # leave the caller's job object (refused when the job does not allow breakaway)
        subprocess.Popen(args, creationflags=base | 0x01000000, **kw)
        return "breakaway"
    except OSError:
        subprocess.Popen(args, creationflags=base, **kw)
        return "none"


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
                                             (not st.get("childPid") and time.time() - st["started"] < 60))
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
    if status == "lost":
        rep["hint"] = "the build worker ended without an exit code (killed?); build again (the build resumes from what was compiled)"
    return rep


def cmd_build(a):
    proj, meta = load_project(a.project)
    target = a.target or default_target(proj)
    test_cmd = os.environ.get("EWTOOL_TEST_UBT")  # tests only: a JSON command list that stands in for UnrealBuildTool
    if test_cmd:
        cmd = json.loads(test_cmd)
    else:
        engine = find_engine(meta, a.engine)
        ubt = engine / "Engine" / "Binaries" / "DotNET" / "UnrealBuildTool" / ("UnrealBuildTool.exe" if IS_WIN else "UnrealBuildTool")
        if not ubt.is_file():
            raise Fail("NO_UBT", f"UnrealBuildTool not found: {ubt}")
        cmd = [str(ubt), target, a.platform, a.config, f"-Project={proj}", "-WaitMutex", "-NoHotReloadFromIDE"] + list(a.ubt_arg or [])
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
    st_path = build_state_path(proj, bid)
    write_state(st_path, {"id": bid, "target": target, "cmd": cmd, "log": str(log), "started": time.time(),
                          "exitCode": None, "project": str(proj)})
    py = pathlib.Path(sys.executable)
    pyw = py.with_name("pythonw.exe")  # no console window for the worker
    worker = [str(pyw if IS_WIN and pyw.is_file() else py), str(HERE / "ewtool.py"), "_worker", str(st_path)]
    how = spawn_detached(worker, proj.parent)
    update_state(st_path, detached=how)
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


# ---------------------------------------------------------------- CLI

def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["_worker"]:
        return worker_main(argv[1])
    ap = argparse.ArgumentParser(prog="ewtool.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("build", help="compile a target")
    p.add_argument("--project", required=True, help="the .uproject file (or its folder)")
    p.add_argument("--engine", help="engine root (default: from EngineAssociation, or UE_ENGINE_DIR)")
    p.add_argument("--target", help="build target (default <Project>Editor)")
    p.add_argument("--platform", default="Win64" if IS_WIN else "Linux")
    p.add_argument("--config", default="Development")
    p.add_argument("--wait", type=int, default=3600, help="seconds to wait for the build; UBT keeps running after it")
    p.add_argument("--lock-wait", type=int, default=120, help="seconds to wait for locked binaries to be released")
    p.add_argument("--ubt-arg", action="append", help="extra UnrealBuildTool argument (repeatable), e.g. --ubt-arg=-ForceUnity")
    p.add_argument("--background", action="store_true", help="start and return at once (follow with status)")
    p = sub.add_parser("status", help="state of a build")
    p.add_argument("--project", required=True)
    p.add_argument("--id", default="latest")
    p.add_argument("--wait", type=int, default=0, help="wait up to N seconds for it to finish")
    p = sub.add_parser("locks", help="who holds the project's binaries")
    p.add_argument("--project", required=True)
    p.add_argument("--file", action="append", help="check this file instead of the project binaries (repeatable)")

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
    except Fail as e:
        emit({"ok": False, "error": e.code, "message": e.message, **e.extra}, 2)


if __name__ == "__main__":
    main()
