"""Build-loop tests: Live Coding, automation tests, and the full C++ cycle
(close editor -> build -> reopen -> reconnect -> build_status).

This suite restarts the editor of ENGINEWRIGHT_TEST_PROJECT, so run_all.py skips it unless named:
    python tests/run_all.py build
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import call as c, check, done, ensure_stopped, rpc, wait_server

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

done()
