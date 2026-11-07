"""Tiny Enginewright client used by tests.

Set ENGINEWRIGHT_TEST_PROJECT to the folder of the Unreal project that runs the plugin
(the token is read from <project>/Saved/Enginewright/token). Optional: ENGINEWRIGHT_URL.
"""
import json, os, pathlib, sys, urllib.request

_proj = os.environ.get("ENGINEWRIGHT_TEST_PROJECT")
if not _proj:
    sys.exit("Set ENGINEWRIGHT_TEST_PROJECT to your Unreal project folder (see README > Running the tests).")
PROJ = pathlib.Path(_proj)
URL = os.environ.get("ENGINEWRIGHT_URL", "http://127.0.0.1:3100/mcp")
TOKEN = (PROJ / "Saved/Enginewright/token").read_text().strip()
_n = 0


def rpc(method, params=None):
    global _n
    _n += 1
    req = urllib.request.Request(URL, method="POST", data=json.dumps({"jsonrpc": "2.0", "id": _n, "method": method, "params": params or {}}).encode(),
                                 headers={"Content-Type": "application/json", "X-Enginewright-Token": TOKEN})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read() or b"{}")


def call(name, args=None):
    return rpc("tools/call", {"name": "call", "arguments": {"name": name, "args": args or {}}})["result"]["structuredContent"]
