"""Shared helpers for the Enginewright tests (Python 3 standard library only).

Set ENGINEWRIGHT_TEST_PROJECT to the folder of an Unreal project that runs the plugin, with that
project open in the editor. The token is read from <project>/Saved/Enginewright/token.
Optional: ENGINEWRIGHT_URL (default http://127.0.0.1:3100/mcp).

Tests write only under /Game/_EWTest and remove what they add to the open level.
"""
import json, os, pathlib, sys, time, urllib.error, urllib.request
from typing import Any

ROOT = "/Game/_EWTest"


def project_dir():
    p = os.environ.get("ENGINEWRIGHT_TEST_PROJECT")
    if not p:
        sys.exit("Set ENGINEWRIGHT_TEST_PROJECT to your Unreal project folder (see README > Running the tests).")
    return pathlib.Path(p)


PROJ = project_dir()
URL = os.environ.get("ENGINEWRIGHT_URL", "http://127.0.0.1:3100/mcp")
_n = 0


def token():
    return (PROJ / "Saved/Enginewright/token").read_text().strip()


def rpc(method, params=None, tok=None, timeout=300) -> tuple[int, Any]:
    global _n
    _n += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _n, "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers={
        "Content-Type": "application/json", "Accept": "application/json, text/event-stream",
        "X-Enginewright-Token": tok if tok is not None else token()})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def call_raw(name, args=None, timeout=300) -> Any:
    """Returns the full tools/call result (content blocks + structuredContent)."""
    s, r = rpc("tools/call", {"name": "call", "arguments": {"name": name, "args": args or {}}}, timeout=timeout)
    if s != 200:
        raise RuntimeError(f"HTTP {s}: {r}")
    return r["result"]


def call(name, args=None, timeout=300) -> Any:
    return call_raw(name, args, timeout)["structuredContent"]


def wait_server(timeout=600):
    """Waits until the editor answers and its start-up asset scan is done. Returns seconds waited."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s, _ = rpc("ping", timeout=5)
            if s == 200 and not call("editor_state", timeout=10)["editor"].get("assetsLoading"):
                return time.time() - t0
        except Exception:
            pass
        time.sleep(2)
    raise TimeoutError(f"Editor did not answer on {URL} within {timeout}s")


_results = []


def check(label, cond, detail: Any = ""):
    _results.append((label, bool(cond)))
    print(("PASS " if cond else "FAIL ") + label + ("" if cond else "  -> " + json.dumps(detail, default=str)[:700]))
    sys.stdout.flush()
    return bool(cond)


def done():
    passed = sum(ok for _, ok in _results)
    print(f"\n{passed}/{len(_results)} passed")
    sys.exit(0 if passed == len(_results) else 1)


def exists(path):
    return call("asset_exists", {"path": path})["data"]["exists"]


def delete_if_exists(*paths):
    """Deletes the given test assets in one call (so references between them do not block or reload them)."""
    there = [p for p in paths if exists(p)]
    if there:
        r = call("asset_delete", {"paths": there, "force": True})
        if not r["ok"]:
            raise RuntimeError(f"cleanup of {there} failed: {r}")


def ensure_stopped():
    wait_server(600)
    if call("editor_state")["data"]["playing"]:
        call("pie_stop")
        for _ in range(100):
            if not call("editor_state")["data"]["playing"]:
                break
            time.sleep(0.2)


def fixtures():
    """Creates the shared fixtures with Enginewright's own tools (so a clean project works):
    E_EWCategory (enum), S_EWItem (row struct), BP_EWTest (Actor with ItemID + Weight)."""
    ensure_stopped()
    enum, struct, bp = ROOT + "/E_EWCategory", ROOT + "/S_EWItem", ROOT + "/BP_EWTest"
    if not exists(enum):
        r = call("enum_create", {"path": enum, "values": ["RawIngredient", "Dish", "Tool"]})
        assert r["ok"], r
    if not exists(bp):
        r = call("blueprint_create", {"path": bp, "parent": "Actor"})
        assert r["ok"], r
        r = call("blueprint_add_variables", {"path": bp, "variables": [
            {"name": "ItemID", "type": "name", "default": "Apple"},
            {"name": "Weight", "type": "float", "default": "1.5"}]})
        assert r["ok"], r
    if not exists(struct):
        r = call("struct_create", {"path": struct, "fields": [
            {"name": "DisplayName", "type": "text"},
            {"name": "Category", "type": "enum:" + enum},
            {"name": "ItemClass", "type": "softclass:Actor"},
            {"name": "ProcessedItemID", "type": "name"},
            {"name": "Weight", "type": "float", "default": "1.0"}]})
        assert r["ok"], r
    return {"enum": enum, "struct": struct, "bp": bp}
