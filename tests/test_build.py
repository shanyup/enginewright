"""Build-loop tests: Live Coding, automation tests, and the full C++ cycle
(close editor -> build -> reopen -> reconnect -> build_status).

This suite restarts the editor of ENGINEWRIGHT_TEST_PROJECT, so run_all.py skips it unless named:
    python tests/run_all.py build
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import PROJ, call as c, check, done, ensure_stopped, rpc, wait_server

ensure_stopped()

# --- automation tests
r = c("automation_list", {"filter": "System.Core.Math.Evaluate"})
check("automation_list finds the engine math tests", r["ok"] and r["data"]["matched"] >= 3, r)
r = c("automation_run", {"filter": "System.Core.Math.Evaluate", "timeout": 120})
check("automation_run reports each test", r["ok"] and r["data"]["passed"] >= 3 and r["data"]["failed"] == 0, r)
r = c("automation_run", {"filter": "No.Such.Test.EW", "timeout": 60})
check("automation_run with no match -> NO_TESTS", not r["ok"] and r["errorCode"] == "NO_TESTS", r)

# --- live coding (no source change: expect noChanges or success)
r = c("live_coding_compile", timeout=900)
check("live_coding_compile returns a result", r["ok"] and r["data"]["result"] in ("noChanges", "success"), r)

# --- ER-07: after a real Live Coding patch, game_run refuses (a new game process would load the old DLL from disk).
# Needs a C++ test project (Source/<module>/*.cpp); skipped on a Blueprint-only project. The source file is restored.
src = next(iter(sorted((PROJ / "Source").glob("*/*.cpp"))), None) if (PROJ / "Source").is_dir() else None
if src:
    original = src.read_bytes()
    try:
        time.sleep(1.5)
        src.write_bytes(original + b"\r\nstatic int EWLiveCodingProbe() { return 7; }\r\n")
        r = c("game_run", {"url": "/Engine/Maps/Templates/Template_Default", "dryRun": True})
        check("C++ edited after the last build -> warning before game_run", r["ok"] and r["data"]["checks"].get("sourceNewerThanBuild") is True
              and any("changed after the last build" in w for w in r.get("warnings", [])), r)
        r = c("live_coding_compile", timeout=900)
        check("live coding patches the edited .cpp", r["ok"] and r["data"]["result"] == "success", r)
        r = c("game_run", {"url": "/Engine/Maps/Templates/Template_Default", "timeout": 60})
        check("game_run after a Live Coding patch -> LIVE_CODING_PATCHED, nothing started", not r["ok"] and r.get("errorCode") == "LIVE_CODING_PATCHED"
              and "editor_restart" in r.get("message", "") and r["data"]["liveCodingPatches"] >= 1, r)
        r = c("game_run", {"url": "/Engine/Maps/Templates/Template_Default", "allowLiveCodingPatch": True, "dryRun": True})
        check("allowLiveCodingPatch -> runs, with a warning", r["ok"] and any("Live Coding patched" in w for w in r.get("warnings", [])), r)
    finally:
        src.write_bytes(original)
else:
    print("SKIP Live Coding / game_run check (the test project has no C++ module)")

# --- restart refusals
c("actor_spawn", {"class": "PointLight", "label": "EW_DirtyMarker"})
r = c("editor_restart", {"build": True})
check("editor_restart refuses with unsaved changes", not r["ok"] and r["errorCode"] == "UNSAVED_CHANGES", r)

# --- full cycle: close, build, reopen, reconnect
t0 = time.time()
r = c("editor_restart", {"build": True, "discardUnsaved": True})
check("editor_restart accepted", r["ok"] and r["data"]["build"], r)
down = False
for _ in range(120):
    time.sleep(1)
    try:
        s, _ = rpc("ping", timeout=3)
        if s != 200:
            down = True
            break
    except Exception:
        down = True
        break
check("editor went down", down)
waited = wait_server(1200)
check("editor came back and answers", True, waited)
r = c("build_status")
check("build_status: succeeded with no errors", r["ok"] and r["data"]["state"] == "succeeded" and not r["data"]["errors"], r)
r = c("actor_find", {"label": "EW_DirtyMarker"})
check("discarded change is gone after restart", r["ok"] and r["data"]["total"] == 0, r)
print(f"(restart cycle took {time.time() - t0:.0f}s)")
r = c("game_run", {"url": "/Engine/Maps/Templates/Template_Default", "dryRun": True})
check("after the rebuild: no Live Coding patch, source not newer than the build", r["ok"] and r["data"]["checks"].get("liveCodingPatches") == 0
      and r["data"]["checks"].get("sourceNewerThanBuild") in (False, None), r)

done()
