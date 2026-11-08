"""Tests for Plugins/Enginewright/Scripts/ewtool.py (build / status / locks), the optional command line helper
for compiling while the editor is closed.

Runs without the editor. ER-05 checks use a stand-in for UnrealBuildTool (EWTOOL_TEST_UBT): the caller of
`build --wait` is killed the way agent tools kill a command (taskkill /T on its process tree, and closing a
KILL_ON_JOB_CLOSE job object that forbids breakaway); the build must keep running and finish.
The real build checks use UnrealBuildTool on ENGINEWRIGHT_TEST_PROJECT (it needs a C++ editor target). If the
project's editor is open, the build is expected to be refused with the editor named as the lock holder.
Everything is written under <project>/Saved/Enginewright/ and the system temp folder.
"""
import json, os, pathlib, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import PROJ, check, done

HERE = pathlib.Path(__file__).resolve().parent
TOOL = HERE.parent / "Plugins" / "Enginewright" / "Scripts" / "ewtool.py"
UPROJECT = next(PROJ.glob("*.uproject"))
IS_WIN = os.name == "nt"
# ER-05 checks use a throw-away project (no binaries, so an open editor of the test project does not lock it).
FAKE_DIR = pathlib.Path(tempfile.mkdtemp(prefix="ewtool_er05_"))
FAKE_PROJ = FAKE_DIR / "EWFake.uproject"
FAKE_PROJ.write_text('{"FileVersion": 3, "EngineAssociation": "5.8"}', encoding="utf-8")


def ew(*args, env=None, timeout=1800):
    e = dict(os.environ, **(env or {}))
    p = subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, encoding="utf-8",
                       errors="replace", env=e, timeout=timeout)
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        return p.returncode, {"raw": p.stdout[-2000:], "stderr": p.stderr[-2000:]}


# A stand-in build: prints UBT-like progress for ~12 s, then succeeds.
FAKE_UBT = json.dumps([sys.executable, "-c",
                       "import time\nfor i in range(1, 7):\n    print(f'[{i}/6] Compile Module.Fake.{i}.cpp', flush=True); time.sleep(2)\n"
                       "print('Result: Succeeded', flush=True)"])


def newest_state():
    files = sorted((FAKE_DIR / "Saved/Enginewright/runs").glob("build-*.json"), key=lambda f: f.stat().st_mtime)
    return files[-1] if files else None


def start_caller(extra_flags=0):
    before = newest_state()
    p = subprocess.Popen([sys.executable, str(TOOL), "build", "--project", str(FAKE_PROJ), "--target", "EWFakeEditor", "--wait", "600", "--lock-wait", "0"],
                         env=dict(os.environ, EWTOOL_TEST_UBT=FAKE_UBT), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=extra_flags)
    return p, before


def wait_for_child(before, timeout=60):
    """Waits until the new build's worker has started the build command. Returns the state file."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        st = newest_state()
        if st and st != before:
            try:
                d = json.loads(st.read_text(encoding="utf-8"))
                if d.get("childPid"):
                    return st, d
            except ValueError:
                pass
        time.sleep(0.3)
    return None, {}


def build_id(state_file):
    return state_file.name[len("build-"):-len(".json")]


# ---------------------------------------------------------------- ER-05: the build outlives its caller
if IS_WIN:
    import ctypes
    from ctypes import wintypes as wt

    # 1) Caller killed with its process tree (taskkill /T /F), as an agent tool does on its time limit.
    caller, before = start_caller()
    st, d = wait_for_child(before)
    check("ER-05 setup: build worker started the build command", st is not None, d)
    if st:
        check("worker is detached outside the caller's tree and job (detached: wmi)", d.get("detached") == "wmi", d)
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(caller.pid)], capture_output=True)
        time.sleep(1)
        check("caller is gone after taskkill /T", caller.poll() is not None)
        code, r = ew("status", "--project", str(FAKE_PROJ), "--id", build_id(st), "--wait", "120")
        check("ER-05: caller tree killed -> build keeps running and succeeds", code == 0 and r.get("status") == "succeeded"
              and r.get("actions") == "6/6", r)

    # 2) Caller inside a KILL_ON_JOB_CLOSE job that forbids breakaway; the job is closed (killing everything in it).
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateJobObjectW.restype = wt.HANDLE

    class BASIC(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64), ("LimitFlags", wt.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t), ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wt.DWORD), ("Affinity", ctypes.c_size_t), ("PriorityClass", wt.DWORD),
                    ("SchedulingClass", wt.DWORD)]

    class IOC(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in ("R", "W", "O", "RT", "WT", "OT")]

    class EXT(ctypes.Structure):
        _fields_ = [("Basic", BASIC), ("Io", IOC), ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    job = k.CreateJobObjectW(None, None)
    info = EXT()
    info.Basic.LimitFlags = 0x2000  # KILL_ON_JOB_CLOSE, no BREAKAWAY_OK
    k.SetInformationJobObject(wt.HANDLE(job), 9, ctypes.byref(info), ctypes.sizeof(info))
    caller, before = start_caller(extra_flags=0x00000004)  # CREATE_SUSPENDED: join the job before it runs
    assigned = k.AssignProcessToJobObject(wt.HANDLE(job), wt.HANDLE(int(caller._handle)))
    ctypes.WinDLL("ntdll").NtResumeProcess(wt.HANDLE(int(caller._handle)))
    check("ER-05 setup: caller runs inside a job without breakaway", bool(assigned))
    st, d = wait_for_child(before)
    check("ER-05 setup: build worker started (job case)", st is not None, d)
    if st:
        k.CloseHandle(wt.HANDLE(job))  # kills every process in the job
        time.sleep(1)
        check("caller was killed by closing its job", caller.poll() is not None)
        code, r = ew("status", "--project", str(FAKE_PROJ), "--id", build_id(st), "--wait", "120")
        check("ER-05: caller's job closed -> build keeps running and succeeds", code == 0 and r.get("status") == "succeeded", r)

    # 3) Old behaviour for comparison (v0.3 candidate, ER-05): a worker started by the caller itself (even with
    #    CREATE_BREAKAWAY_FROM_JOB) dies with the job; status must say "lost", not "running".
    job = k.CreateJobObjectW(None, None)
    k.SetInformationJobObject(wt.HANDLE(job), 9, ctypes.byref(info), ctypes.sizeof(info))
    before = newest_state()
    caller = subprocess.Popen([sys.executable, str(TOOL), "build", "--project", str(FAKE_PROJ), "--target", "EWFakeEditor", "--wait", "600", "--lock-wait", "0"],
                              env=dict(os.environ, EWTOOL_TEST_UBT=FAKE_UBT, EWTOOL_SPAWN="plain"),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=0x00000004)
    k.AssignProcessToJobObject(wt.HANDLE(job), wt.HANDLE(int(caller._handle)))
    ctypes.WinDLL("ntdll").NtResumeProcess(wt.HANDLE(int(caller._handle)))
    st, d = wait_for_child(before)
    if st:
        check("comparison run uses the old in-process spawn (no WMI)", d.get("detached") in ("none", "breakaway"), d)
        k.CloseHandle(wt.HANDLE(job))
        time.sleep(2)
        code, r = ew("status", "--project", str(FAKE_PROJ), "--id", build_id(st))
        check("a build killed with its caller is reported as lost (with a hint), not as running", code == 1 and r.get("status") == "lost"
              and r.get("hint"), r)
else:
    print("SKIP ER-05 job/tree checks (Windows only)")

# ---------------------------------------------------------------- locks and build (UnrealBuildTool)
code, r = ew("locks", "--project", str(UPROJECT))
check("locks -> report", code == 0 and r.get("ok") and r.get("filesChecked", 0) > 0, r)
editor_open = bool(r.get("locked")) and any("unrealeditor" in h["name"].lower() for h in r.get("holders", []))
if editor_open:
    code, r = ew("build", "--project", str(UPROJECT), "--lock-wait", "0")
    check("editor open -> build refused LOCKED, editor named", code == 2 and r.get("error") == "LOCKED"
          and any("unrealeditor" in h["name"].lower() and h.get("files") for h in r.get("holders", [])), r)
    print("NOTE editor is open: real build checks skipped (close the editor to run them)")
else:
    dll = next((f for f in (PROJ / "Binaries").rglob("*.dll")), None)
    if dll:
        holder = subprocess.Popen([sys.executable, "-c", f"f=open(r'{dll}','rb'); import time; time.sleep(120)"])
        time.sleep(1.5)
        code, r = ew("build", "--project", str(UPROJECT), "--lock-wait", "0")
        hs = r.get("holders", [])
        # (a venv python.exe is a launcher: the file may be held by its child, so match the command line too)
        check("binary held by another process -> LOCKED naming that process and file", code == 2 and r.get("error") == "LOCKED"
              and any((h["pid"] == holder.pid or "time.sleep(120)" in h.get("commandLine", "")) and any(dll.name in f for f in h.get("files", [])) for h in hs), r)
        holder.kill()
        holder.wait()
    t0 = time.time()
    code, r = ew("build", "--project", str(UPROJECT), "--background")
    bid = r.get("id")
    check("build --background returns at once with an id", code in (0, 3) and bid and time.time() - t0 < 60, r)
    code, r = ew("status", "--project", str(UPROJECT), "--id", str(bid), "--wait", "1500")
    check("status follows the detached build to the end -> succeeded", code == 0 and r.get("status") == "succeeded"
          and r.get("result", "").startswith("Succeeded"), r)
    code, r = ew("build", "--project", str(UPROJECT), "--wait", "1500")
    check("build again -> up to date, succeeded", code == 0 and r.get("status") == "succeeded" and r.get("upToDate"), r)
code, r = ew("status", "--project", str(UPROJECT), "--id", "nope")
check("status of an unknown build -> NO_BUILD", code == 2 and r.get("error") == "NO_BUILD", r)
code, r = ew("build", "--project", str(PROJ / "nope.uproject"))
check("missing project -> NO_PROJECT", code == 2 and r.get("error") == "NO_PROJECT", r)
code, r = ew("run", "--project", str(UPROJECT))
check("run/test/compose are MCP tools now (game_run, image_compose): ewtool refuses them", code == 2, r)

time.sleep(1)
shutil.rmtree(FAKE_DIR, ignore_errors=True)
done()
