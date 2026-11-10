"""Smoke test: protocol, auth, meta tools, strict arguments, client configs. Read-only; safe on any project.

Usage: python tests/smoke.py   (ENGINEWRIGHT_TEST_PROJECT must point at the open project)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import rpc, call as c, check, done

try:
    s, _ = rpc("ping", tok="wrong")
except RuntimeError as e:
    print(e, file=sys.stderr)
    sys.exit(2)
check("auth: wrong token is refused (401)", s == 401, s)
s, r = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "0"}})
check("initialize", s == 200 and r["result"]["serverInfo"]["name"] == "enginewright", r)
s, r = rpc("tools/list")
check("tools/list = 3 meta tools", [t["name"] for t in r["result"]["tools"]] == ["search", "describe", "call"], r)
s, r = rpc("tools/call", {"name": "search", "arguments": {"query": "set property"}})
check("search finds object_set first", r["result"]["structuredContent"]["data"]["results"][0]["name"] == "object_set", r)
s, r = rpc("tools/call", {"name": "search", "arguments": {"query": "screenshot game camera"}})
check("search finds screenshot", r["result"]["structuredContent"]["data"]["results"][0]["name"] == "screenshot", r)
s, r = rpc("tools/call", {"name": "describe", "arguments": {"name": "log_wait"}})
check("describe marks waiting tools", r["result"]["structuredContent"]["data"]["waits"] is True, r)
s, r = rpc("tools/call", {"name": "describe", "arguments": {"name": "objet_get"}})
check("describe unknown tool -> suggestion", "object_get" in r["result"]["structuredContent"].get("suggestions", []), r)

r = c("editor_state")
check("editor_state ok", r["ok"] and "playing" in r["data"] and r["data"]["server"].startswith("Enginewright"), r)
r = c("editor_state", {"bogus": 1})
check("unknown argument -> error", not r["ok"] and r["errorCode"] == "UNKNOWN_ARGUMENT", r)
r = c("console_commnd", {"command": "stat fps"})
check("unknown tool -> suggestion", not r["ok"] and "console_command" in r.get("suggestions", []), r)
r = c("console_command", {"command": "stat fps"})
check("console_command editor", r["ok"], r)
r = c("console_command", {})
check("missing required argument -> error", not r["ok"] and r["errorCode"] == "MISSING_ARGUMENT", r)
r = c("screenshot", {"source": "camera"})
check("bad enum argument -> error with suggestion", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("batch", {"steps": [{"tool": "log_wait", "args": {"pattern": "x"}}]})
check("waiting tool inside batch -> refused", not r["ok"], r)

r = c("client_config")
names = [x["client"] for x in r["data"]["configs"]] if r["ok"] else []
check("client_config lists 6 configs", r["ok"] and names == ["claude-code", "claude-code-cli", "codex", "cursor", "vscode", "hermes"], r)
check("client config files exist on disk", r["ok"] and all(os.path.isfile(os.path.join(r["data"]["folder"], x["file"])) for x in r["data"]["configs"]), r)
r = c("client_config", {"client": "codex"})
check("codex config is TOML with url and header", r["ok"] and len(r["data"]["configs"]) == 1 and "[mcp_servers.enginewright]" in r["data"]["configs"][0]["text"], r)

done()
