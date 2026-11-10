"""Play In Editor tests: start/stop, state, game camera screenshot, synthetic input, pie_walk_to, pie_travel, log wait,
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

r = c("pie_travel", {"map": "/Engine/Maps/Templates/Template_Default"})
check("pie_travel without PIE -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)
r = c("pie_walk_to", {"x": 0, "y": 0})
check("pie_walk_to without PIE -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)

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
# pie_walk_to (ER-22): the pawn walks to a position with movement input
def loc():
    return c("pie_state")["data"]["worlds"][0]["players"][0]["location"]


def dist2d(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


r = c("pie_walk_to", {"y": 0})
check("pie_walk_to without x -> MISSING_ARGUMENT", not r["ok"] and r["errorCode"] == "MISSING_ARGUMENT", r)
r = c("pie_walk_to", {"x": 0, "y": 0, "mode": "fly"})
check("pie_walk_to bad mode -> error", not r["ok"] and r["errorCode"] in ("INVALID_ARGUMENT", "SCHEMA", "INVALID_VALUE"), r)
r = c("pie_walk_to", {"x": 0, "y": 0, "acceptRadius": 0})
check("pie_walk_to acceptRadius 0 -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_walk_to", {"x": 0, "y": 0, "timeout": 0})
check("pie_walk_to timeout 0 -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_walk_to", {"x": 0, "y": 0, "stuckSeconds": 0.1})
check("pie_walk_to stuckSeconds below 0.5 -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_walk_to", {"x": 0, "y": 0, "player": 3})
check("pie_walk_to missing player -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)

start = loc()
target = [start[0] + 600, start[1] + 300]
r = c("pie_walk_to", {"x": target[0], "y": target[1], "timeout": 30, "mode": "direct"}, timeout=60)
end = loc()
check("pie_walk_to reaches the target (data and pie_state agree)", r["ok"] and r["data"]["status"] == "reached" and r["data"]["reached"] is True
      and r["data"]["distance"] <= 80 and dist2d(end, target) < 130 and r["data"]["mode"] == "direct", {"r": r, "end": end, "target": target})
check("pie_walk_to reports location, target and seconds", r["ok"] and len(r["data"]["location"]) == 3 and len(r["data"]["target"]) == 3 and r["data"]["seconds"] > 0.1, r)
r = c("pie_walk_to", {"x": end[0], "y": end[1], "acceptRadius": 500})
check("pie_walk_to when already inside acceptRadius returns at once", r["ok"] and r["data"]["status"] == "reached" and r["data"]["seconds"] < 1.0, r)
r = c("pie_walk_to", {"x": end[0] + 900000, "y": end[1], "timeout": 1, "stuckSeconds": 30}, timeout=30)
check("pie_walk_to timeout -> TIMEOUT with the last location", not r["ok"] and r["errorCode"] == "TIMEOUT" and r["data"]["status"] == "timeout"
      and r["data"]["reached"] is False and r["data"]["distance"] > 1000, r)
r = c("pie_walk_to", {"x": end[0] + 500, "y": end[1], "mode": "navmesh", "timeout": 5}, timeout=30)
check("pie_walk_to mode navmesh without a navmesh -> NO_NAVMESH (or walks when the level has one)", (not r["ok"] and r["errorCode"] == "NO_NAVMESH") or (r["ok"] and r["data"]["mode"].startswith("navmesh")), r)
c("console_command", {"command": "slomo 0.0001"})
r = c("pie_walk_to", {"x": end[0] + 5000, "y": end[1], "timeout": 20, "stuckSeconds": 1}, timeout=40)
c("console_command", {"command": "slomo 1"})
check("pie_walk_to detects a pawn that does not move -> STUCK with the position", not r["ok"] and r["errorCode"] == "STUCK" and r["data"]["status"] == "stuck"
      and len(r["data"]["location"]) == 3 and "did not move" in r["message"], r)

r = c("pie_input", {"key": "Spacebarr"})
check("unknown key -> suggestion", not r["ok"] and r["errorCode"] == "UNKNOWN_KEY" and "SpaceBar" in r.get("suggestions", []), r)
r = c("pie_input", {"key": "W", "action": "/Game/X"})
check("key and action together -> error", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_input", {"key": "W", "player": 3})
check("missing player index -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)

# --- pie_travel (ER-26): a map plus URL options, checked; waits until the new map is loaded
TRAVEL = "/Engine/Maps/Templates/Template_Default"
r = c("pie_travel", {"map": ""})
check("pie_travel empty map -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_travel", {})
check("pie_travel without map -> MISSING_ARGUMENT", not r["ok"] and r["errorCode"] == "MISSING_ARGUMENT", r)
r = c("pie_travel", {"map": "Template_Defaul"})
check("pie_travel unknown map -> MAP_NOT_FOUND with the closest name", not r["ok"] and r["errorCode"] == "MAP_NOT_FOUND" and "Template_Default" in r.get("suggestions", []), r)
r = c("pie_travel", {"map": "/Game/NoSuchMap"})
check("pie_travel unknown map path -> MAP_NOT_FOUND", not r["ok"] and r["errorCode"] == "MAP_NOT_FOUND", r)
r = c("pie_travel", {"map": TRAVEL, "options": "?bad option"})
check("pie_travel option with a space -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT" and "bad option" in r["message"], r)
r = c("pie_travel", {"map": TRAVEL, "options": "?=x"})
check("pie_travel option without a key -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_travel", {"map": TRAVEL, "timeout": 0})
check("pie_travel timeout 0 -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_travel", {"map": "Template_Default", "options": "?listen"}, timeout=240)
d = r.get("data") or {}
w0 = (d.get("worlds") or [{}])[0]
check("pie_travel by map name + ?listen: loads the map, URL reported, pawn ready", r["ok"] and d["travelUrl"] == TRAVEL + "?listen" and d["waited"] is True
      and w0.get("map", "").endswith("Template_Default") and d["localPlayersWithPawn"] >= 1 and d["seconds"] > 0, r)
check("pie_travel: the ?listen option took effect (the world is a listen server now)", w0.get("netMode") == "listenServer", w0)
r = c("pie_travel", {"map": TRAVEL, "options": "listen,foo=1"}, timeout=240)
d = r.get("data") or {}
check("pie_travel: options without ? and with a comma are normalised (travel to the same map again)", r["ok"] and d.get("travelUrl") == TRAVEL + "?listen?foo=1"
      and d["worlds"][0]["map"].endswith("Template_Default") and d["localPlayersWithPawn"] >= 1, r)
r = c("pie_travel", {"map": TRAVEL, "wait": False})
check("pie_travel wait false returns at once with the URL", r["ok"] and r["data"]["travelUrl"] == TRAVEL and r["data"]["waited"] is False, r)
time.sleep(2)
n = 0
while n < 60 and not (c("pie_state")["data"].get("localPlayersWithPawn", 0) >= 1 and c("pie_state")["data"]["worlds"][0]["timeSeconds"] < 20):
    time.sleep(1); n += 1
check("pie_travel wait false: the map is up a moment later", n < 60, n)

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
