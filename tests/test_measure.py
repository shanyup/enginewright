"""Measuring tests: class_measure (temporary spawn, level unchanged) and screen_coverage (PIE readability).

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT whose open level has a player start or default pawn.
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, done, ensure_stopped, exists, fixtures

fx = fixtures()
ensure_stopped()
CUBE = ROOT + "/BP_EWMeasure"
LABEL = "EW_MeasureCube"
if not exists(CUBE):
    r = c("blueprint_create", {"path": CUBE, "parent": "Actor"})
    assert r["ok"], r
    r = c("blueprint_add_component", {"path": CUBE, "class": "StaticMeshComponent", "name": "Body",
                                      "values": {"StaticMesh": "/Engine/BasicShapes/Cube.Cube"}})
    assert r["ok"], r
c("actor_delete", {"actors": [LABEL]})

# --- class_measure
before = c("actor_find", {"limit": 1000})["data"]["total"]
r = c("class_measure", {"class": CUBE})
size = r["data"]["size"] if r["ok"] else []
check("class_measure: engine cube is 100 x 100 x 100", r["ok"] and len(size) == 3 and all(abs(s - 100) < 1 for s in size), r)
check("class_measure: component listed with its size", r["ok"] and any(x["name"].startswith("Body") and abs(x["size"][2] - 100) < 1 for x in r["data"]["components"]), r)
check("class_measure leaves no actor behind", c("actor_find", {"limit": 1000})["data"]["total"] == before)
r = c("class_measure", {"class": fx["bp"]})
check("class_measure: no components -> zero size + warning", r["ok"] and r["data"]["size"] == [0, 0, 0] and r.get("warnings"), r)
r = c("class_measure", {"class": "BP_NoSuchThing"})
check("class_measure unknown class -> UNKNOWN_CLASS", not r["ok"] and r["errorCode"] == "UNKNOWN_CLASS", r)
r = c("class_measure", {"class": "StaticMeshComponent"})
check("class_measure non-actor -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)

# --- screen_coverage
r = c("screen_coverage", {"actor": LABEL})
check("screen_coverage without PIE -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)
r = c("pie_start", {"timeout": 90})
cam = r["data"]["worlds"][0]["players"][0] if r["ok"] else None
c("pie_stop")
if cam:
    pitch, yaw = math.radians(cam["cameraRotation"][0]), math.radians(cam["cameraRotation"][1])
    fwd = [math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch)]
    loc = [cam["cameraLocation"][i] + fwd[i] * 500 for i in range(3)]
    s = c("actor_spawn", {"class": CUBE, "label": LABEL, "location": loc})
    check("spawn a cube 5 m in front of the player camera", s["ok"], s)
    r = c("pie_start", {"timeout": 90})
    check("pie_start", r["ok"], r)
    r = c("screen_coverage", {"actor": LABEL})
    d = r.get("data") or {}
    check("screen_coverage: cube on screen, fully visible, sensible size", r["ok"] and d.get("onScreen") and d.get("fullyOnScreen")
          and 0 < d.get("screenPercent", 0) < 50 and d.get("widthPx", 0) > 10 and 400 < d.get("cameraDistance", 0) < 700, r)
    r2 = c("screen_coverage", {"class": CUBE})
    check("screen_coverage by class finds the same actor", r2["ok"] and r2["data"]["actor"] == LABEL, r2)
    r = c("screen_coverage", {"actor": "EW_NoSuchActor"})
    check("screen_coverage unknown actor -> ACTOR_NOT_FOUND", not r["ok"] and r["errorCode"] == "ACTOR_NOT_FOUND", r)
    r = c("screen_coverage", {})
    check("screen_coverage without actor/class -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
    c("pie_stop")
    c("actor_delete", {"actors": [LABEL]})
else:
    check("pie_start for camera position", False, r)

done()
