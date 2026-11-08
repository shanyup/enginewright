"""Multiplayer tests: pie_actors (what each world sees) and pie_net_emulation, in a 2-player listen session.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT whose open level has a player start or default pawn.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import call as c, check, done, ensure_stopped, fixtures

fixtures()
ensure_stopped()
r = c("pie_actors", {"class": "Pawn"})
check("pie_actors without PIE -> PIE_NOT_RUNNING", not r["ok"] and r["errorCode"] == "PIE_NOT_RUNNING", r)

# --- single player: no network
r = c("pie_start", {"timeout": 90})
check("pie_start 1 player", r["ok"], r)
r = c("pie_net_emulation", {"profile": "bad"})
check("net emulation in standalone -> NOT_NETWORKED", not r["ok"] and r["errorCode"] == "NOT_NETWORKED", r)
c("pie_stop")

# --- two players
r = c("pie_start", {"players": 2, "netMode": "listen", "timeout": 120})
check("pie_start 2 players listen", r["ok"], r)
r = c("pie_actors", {"class": "Pawn", "replicatedOnly": True, "minCount": 2, "timeout": 20, "properties": ["BaseEyeHeight"]})
w = (r.get("data") or {}).get("worlds", [])
check("pie_actors: both worlds see 2 pawns", r["ok"] and len(w) == 2 and all(x["count"] == 2 for x in w) and r["data"]["sameCountEverywhere"], r)
client = next((x for x in w if x["netMode"] == "client"), None)
roles = sorted(a["localRole"] for a in client["actors"]) if client else []
check("pie_actors: client sees own pawn (autonomous) and host pawn (simulated)", roles == ["autonomousProxy", "simulatedProxy"], roles)
check("pie_actors: property values read", bool(w) and all("BaseEyeHeight" in a.get("values", {}) for x in w for a in x["actors"]), w)
r = c("pie_actors", {"class": "Pawn", "properties": ["BaseEyeHieght"]})
check("pie_actors unknown property -> suggestion", not r["ok"] and r["errorCode"] == "UNKNOWN_PROPERTY" and "BaseEyeHeight" in r.get("suggestions", []), r)
r = c("pie_actors", {"class": "Pawn", "minCount": 9, "timeout": 1})
check("pie_actors minCount not reached -> TIMEOUT with data", not r["ok"] and r["errorCode"] == "TIMEOUT" and (r.get("data") or {}).get("worlds"), r)
r = c("pie_actors", {"class": "NoSuchClassX"})
check("pie_actors unknown class -> UNKNOWN_CLASS", not r["ok"] and r["errorCode"] == "UNKNOWN_CLASS", r)

r = c("pie_net_emulation", {"profile": "bad"})
d = (r.get("data") or {}).get("drivers", [])
check("net emulation bad: applied to server and client, read back", r["ok"] and len(d) == 2 and all(x["lagMin"] == 100 and x["loss"] == 5 for x in d), r)
r = c("pie_net_emulation", {"lagMin": 20, "lagMax": 40, "loss": 0, "reorder": True})
d = (r.get("data") or {}).get("drivers", [])
check("net emulation custom values", r["ok"] and d and all(x["lagMax"] == 40 and x["reorder"] for x in d), r)
r = c("pie_net_emulation", {"loss": 150})
check("loss 150 -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_net_emulation", {"lagMin": 300, "lagMax": 100})
check("lagMax < lagMin -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("pie_actors", {"class": "Pawn", "replicatedOnly": True, "minCount": 2, "timeout": 20})
check("replication still works under emulated lag", r["ok"], r)
r = c("pie_net_emulation", {"profile": "off"})
check("net emulation off", r["ok"] and all(x["lagMax"] == 0 and x["loss"] == 0 for x in r["data"]["drivers"]), r)
c("pie_stop")

done()
