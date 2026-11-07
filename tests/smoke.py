"""Smoke test for Enginewright v0.1. Run with the editor open.

Usage: python smoke.py [project_dir] [blueprint_path]
project_dir defaults to the ENGINEWRIGHT_TEST_PROJECT environment variable. Optional: ENGINEWRIGHT_URL.
"""
import json, os, pathlib, sys, urllib.request

_proj = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("ENGINEWRIGHT_TEST_PROJECT")
if not _proj:
    sys.exit("Pass the project folder or set ENGINEWRIGHT_TEST_PROJECT (see README > Running the tests).")
PROJ = pathlib.Path(_proj)
URL = os.environ.get("ENGINEWRIGHT_URL", "http://127.0.0.1:3100/mcp")
TOKEN = (PROJ / "Saved/Enginewright/token").read_text().strip()
n = 0


def rpc(method, params=None, token=TOKEN):
    global n
    n += 1
    body = json.dumps({"jsonrpc": "2.0", "id": n, "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers={
        "Content-Type": "application/json", "Accept": "application/json, text/event-stream", "X-Enginewright-Token": token})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def call(name, args=None):
    s, r = rpc("tools/call", {"name": "call", "arguments": {"name": name, "args": args or {}}})
    return r["result"]["structuredContent"]


checks = []
def check(label, cond, detail=""):
    checks.append((label, bool(cond)))
    print(("PASS " if cond else "FAIL ") + label + ("" if cond else f"  -> {str(detail)[:400]}"))


s, _ = rpc("ping", token="wrong")
check("auth: wrong token is refused (401)", s == 401, s)
s, r = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "0"}})
check("initialize", s == 200 and r["result"]["serverInfo"]["name"] == "enginewright", r)
s, r = rpc("tools/list")
check("tools/list = 3 meta tools", [t["name"] for t in r["result"]["tools"]] == ["search", "describe", "call"], r)
s, r = rpc("tools/call", {"name": "search", "arguments": {"query": "set property"}})
check("search finds object_set", r["result"]["structuredContent"]["data"]["results"][0]["name"] == "object_set", r)

r = call("editor_state")
check("editor_state ok", r["ok"] and "playing" in r["data"], r)
r = call("editor_state", {"bogus": 1})
check("unknown argument -> error", not r["ok"] and r["errorCode"] == "UNKNOWN_ARGUMENT", r)
r = call("console_commnd", {"command": "stat fps"})
check("unknown tool -> suggestion", not r["ok"] and "console_command" in r.get("suggestions", []), r)
r = call("console_command", {"command": "stat fps"})
check("console_command editor", r["ok"], r)

# Strict writes on a scratch asset: a DataTable row struct is not a UObject, so test on a material instance-free target:
# the editor's own World Settings object is always there; use a Blueprint CDO when the project passes one.
target = sys.argv[2] if len(sys.argv) > 2 else None
if target:
    r = call("object_get", {"path": target, "classDefault": True})
    check("object_get CDO", r["ok"], r)
    r = call("object_set", {"path": target, "classDefault": True, "values": {"NotAField": 1}, "save": False})
    check("unknown property -> error, nothing written", not r["ok"] and r["errorCode"] == "UNKNOWN_PROPERTY", r)

print(f"\n{sum(c for _, c in checks)}/{len(checks)} passed")
