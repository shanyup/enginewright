"""Play In Editor tests: start/stop, state, game camera screenshot, synthetic input, log wait,
console commands per player, and a 2-player listen-server session.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT whose open level has a player start or default pawn.
"""
import base64, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, call_raw, check, done, ensure_stopped, fixtures

fx = fixtures()
ensure_stopped()

# --- editor viewport screenshot (no PIE)
raw = call_raw("screenshot", {"source": "editor", "maxWidth": 640})
r = raw["structuredContent"]
imgs = [b for b in raw["content"] if b["type"] == "image"]
check("editor screenshot: PNG file + inline image", r["ok"] and os.path.isfile(r["data"]["file"]) and imgs
      and base64.b64decode(imgs[0]["data"])[:8] == b"\x89PNG\r\n\x1a\n" and r["data"]["width"] <= 640, r)
r = c("screenshot", {"source": "game"})
check("game screenshot without PIE -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)
r = c("pie_input", {"key": "SpaceBar"})
check("pie_input without PIE -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)

# --- single player
t0 = time.time()
r = c("pie_start", {"timeout": 90})
check("pie_start waits until the player has a pawn", r["ok"] and r["data"]["localPlayersWithPawn"] == 1, r)
r = c("pie_start")
check("second pie_start -> PIE_RUNNING", not r["ok"] and r["errorCode"] == "PIE_RUNNING", r)
r = c("blueprint_create", {"path": ROOT + "/BP_DuringPIE"})
check("write during PIE -> PIE_RUNNING", not r["ok"] and r["errorCode"] == "PIE_RUNNING", r)
st = c("pie_state")
p0 = st["data"]["worlds"][0]["players"][0]
check("pie_state: standalone world, camera + fov", st["ok"] and st["data"]["worlds"][0]["netMode"] == "standalone" and p0["fov"] > 0, st)

raw = call_raw("screenshot", {"maxWidth": 800})
r = raw["structuredContent"]
check("game camera screenshot (default source while playing)", r["ok"] and r["data"]["source"] == "game" and r["data"]["camera"]["fov"] > 0
      and any(b["type"] == "image" for b in raw["content"]), r)
check("game screenshot is not black (scene is lit and drawn)", r["ok"] and r["data"]["meanBrightness"] > 0.03, r.get("data"))

# console command through the player + log wait for its output
seq = c("log_read", {"limit": 1})["data"]["lastSeq"]
r = c("console_command", {"command": "log LogTemp Display"})
check("console_command in PIE goes through player 0", r["ok"] and r["data"]["target"] == "pie" and r["data"]["player"] == 0, r)
c("console_command", {"command": "ke * EW_Ping"})
r = c("console_command", {"command": "say EnginewrightHello"})
r = c("log_wait", {"pattern": "EnginewrightHello", "since": seq, "timeout": 10})
check("log_wait finds the line printed by a PIE command", r["ok"] and "EnginewrightHello" in r["data"]["text"], r)
t = time.time()
r = c("log_wait", {"pattern": "EW_line_that_never_comes", "timeout": 1.5})
check("log_wait timeout -> TIMEOUT after ~1.5 s", not r["ok"] and r["errorCode"] == "TIMEOUT" and 1.0 < time.time() - t < 8, r)

# synthetic key input: the default pawn moves forward on W
before = c("pie_state")["data"]["worlds"][0]["players"][0]["location"]
r = c("pie_input", {"key": "W", "seconds": 1.0})
after = c("pie_state")["data"]["worlds"][0]["players"][0]["location"]
moved = sum((a - b) ** 2 for a, b in zip(after, before)) ** 0.5
check("pie_input key W held 1 s moves the default pawn", r["ok"] and moved > 50, {"r": r, "before": before, "after": after})
r = c("pie_input", {"key": "Spacebarr"})
check("unknown key -> suggestion", not r["ok"] and r["errorCode"] == "UNKNOWN_KEY" and "SpaceBar" in r.get("suggestions", []), r)
r = c("pie_input", {"key": "W", "action": "/Game/X"})
check("key and action together -> error", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_input", {"key": "W", "player": 3})
check("missing player index -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)

r = c("pie_stop")
check("pie_stop waits until stopped", r["ok"] and not c("editor_state")["data"]["playing"], r)

# --- two players, listen server
r = c("pie_start", {"players": 2, "netMode": "listen", "timeout": 120})
check("2-player listen server: both players have pawns", r["ok"] and r["data"]["localPlayersWithPawn"] == 2, r)
if r["ok"]:
    modes = sorted(w["netMode"] for w in r["data"]["worlds"])
    check("worlds are listenServer + client", modes == ["client", "listenServer"], modes)
    r1 = c("console_command", {"command": "stat unit", "player": 1})
    check("console_command on player 1 (client)", r1["ok"] and r1["data"]["player"] == 1, r1)
    shot = c("screenshot", {"player": 1, "inline": False, "maxWidth": 320})
    check("client screenshot", shot["ok"] and os.path.isfile(shot["data"]["file"]), shot)
c("pie_stop")
r = c("pie_start", {"players": 2, "netMode": "standalone"})
check("2 players standalone -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)

done()
