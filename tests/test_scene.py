"""Scene dressing tests: scene_dress (ER-15) and actor_preview (ER-17).

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. The actor_preview checks that run BeginPlay need the C++ fixture
tests/fixtures/EWTestDressActor.* in the test project's module (see the header); without it they FAIL with a message.
Everything created in the level is removed again; assets live under /Game/_EWTest.
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, call_raw, check, delete_if_exists, done, ensure_stopped
import ewpng

ensure_stopped()
FLOOR_BP = ROOT + "/BP_EWDressFloor"
STRUCT = ROOT + "/S_EWDress"
TABLE = ROOT + "/DT_EWDress"
CUBE, SPHERE, CYL = "/Engine/BasicShapes/Cube.Cube", "/Engine/BasicShapes/Sphere.Sphere", "/Engine/BasicShapes/Cylinder.Cylinder"
FLOOR, CENTER = "EW_DressFloor", "EW_DressCenter"
FIXTURE = "/Script/EnginewrightTest.EWTestDressActor"


def level_actors():
    return c("actor_find", {"limit": 5000})["data"]["total"]


def cleanup():
    left = [a["label"] for a in c("actor_find", {"label": "EW_*", "limit": 5000})["data"]["actors"]]
    if left:
        c("actor_delete", {"actors": left})
    delete_if_exists(TABLE, STRUCT, FLOOR_BP)


cleanup()
start_actors = level_actors()

# --- fixtures: a floor to stand on, a row struct like a game's dressing table, the table
r = c("blueprint_create", {"path": FLOOR_BP, "parent": "Actor"})
assert r["ok"], r
r = c("blueprint_add_component", {"path": FLOOR_BP, "class": "StaticMeshComponent", "name": "Slab", "values": {"StaticMesh": CUBE, "RelativeScale3D": {"X": 40, "Y": 40, "Z": 0.2}}})
check("floor Blueprint built", r["ok"], r)
r = c("struct_create", {"path": STRUCT, "fields": [
    {"name": "Scene", "type": "name"}, {"name": "Mesh", "type": "softobject:StaticMesh"}, {"name": "Location", "type": "vector"},
    {"name": "Rotation", "type": "rotator"}, {"name": "Scale", "type": "vector"}, {"name": "bCollide", "type": "bool"}]})
check("row struct created", r["ok"], r)
r = c("datatable_create", {"path": TABLE, "rowStruct": STRUCT})
check("DataTable created", r["ok"], r)
r = c("actor_spawn", {"class": FLOOR_BP, "label": FLOOR, "location": [0, 0, 0]})
check("floor placed", r["ok"], r)
r = c("actor_spawn", {"class": "PointLight", "label": CENTER, "location": [1000, 2000, 300], "rotation": [0, 90, 0]})
check("center actor placed (yaw 90)", r["ok"], r)
base_count = level_actors()


def dress(**kw):
    kw.setdefault("name", "EW_D")
    return c("scene_dress", kw)


# --- ring: count, geometry, one actor, undo-friendly
r = dress(target=CENTER, pattern="ring", meshes=[CUBE], count=12, radius=500, name="EW_Ring")
d = r.get("data") or {}
check("ring: 12 placed, instanced, read back from the level", r["ok"] and d.get("placed") == 12 and d.get("inLevel") == 12 and d.get("output") == "instanced", r)
check("ring: one new actor", level_actors() == base_count + 1, level_actors())
amin, amax = d.get("areaMin", [0, 0, 0]), d.get("areaMax", [0, 0, 0])
check("ring: area is 1000 wide around the target (500 radius)", abs((amax[0] - amin[0]) - 1000) < 60 and abs((amin[0] + amax[0]) / 2 - 1000) < 40 and abs((amin[1] + amax[1]) / 2 - 2000) < 40, d)

# --- replace by name keeps the count steady; replace:false adds
r = dress(target=CENTER, pattern="ring", meshes=[CUBE], count=8, radius=500, name="EW_Ring")
check("same name replaces the earlier result", r["ok"] and r["data"]["replacedActors"] == 1 and level_actors() == base_count + 1, r)
r = dress(target=CENTER, pattern="ring", meshes=[CUBE], count=8, radius=300, name="EW_Ring", replace=False, label="EW_RingKept")
check("replace:false keeps both", r["ok"] and level_actors() == base_count + 2, r)

# --- seed: same seed same result, other seed other result
a = dress(center=[0, 0, 0], pattern="scatter", meshes=[CUBE, SPHERE], count=30, radius=800, seed=7, name="EW_Seed", output="none", export={"table": TABLE, "scene": "S1", "prefix": "seedA", "save": False})
b = dress(center=[0, 0, 0], pattern="scatter", meshes=[CUBE, SPHERE], count=30, radius=800, seed=7, name="EW_Seed", output="none", export={"table": TABLE, "scene": "S1", "prefix": "seedB", "save": False})
e = dress(center=[0, 0, 0], pattern="scatter", meshes=[CUBE, SPHERE], count=30, radius=800, seed=8, name="EW_Seed", output="none", export={"table": TABLE, "scene": "S1", "prefix": "seedC", "save": False})
rows = c("datatable_get", {"path": TABLE})
rd = rows["data"].get("rows", {}) if rows["ok"] else {}


def first_loc(prefix):
    row = rd.get(prefix + "_001") or {}
    return row.get("Location")


check("seed: same seed gives the same first location", first_loc("seedA") is not None and first_loc("seedA") == first_loc("seedB"), (first_loc("seedA"), first_loc("seedB")))
check("seed: another seed differs", first_loc("seedA") != first_loc("seedC"), (first_loc("seedA"), first_loc("seedC")))
check("output none changes no actor", level_actors() == base_count + 2, level_actors())

# --- line, grid
r = dress(center=[0, 0, 0], pattern="line", meshes=[CUBE], count=5, to=[400, 0, 0], name="EW_Line", output="actors")
d = r.get("data") or {}
check("line: 5 actors placed, 400 apart end to end", r["ok"] and d.get("placed") == 5 and abs(d["areaMax"][0] - d["areaMin"][0] - 400) < 1, r)
check("line: actors mode makes 5 actors", level_actors() == base_count + 2 + 5, level_actors())
r = dress(center=[0, 0, 0], pattern="grid", meshes=[CUBE], rows=3, cols=4, spacing=[200, 300], name="EW_Grid")
d = r.get("data") or {}
check("grid: 3 x 4 = 12, 400 by 900 area", r["ok"] and d.get("placed") == 12 and abs(d["areaMax"][0] - d["areaMin"][0] - 400) < 1 and abs(d["areaMax"][1] - d["areaMin"][1] - 900) < 1, r)

# --- weights and cycle
r = dress(center=[0, 0, 0], pattern="line", meshes=[CUBE, SPHERE], count=10, to=[900, 0, 0], pick="cycle", name="EW_Cycle")
pm = (r.get("data") or {}).get("perMesh", {})
check("cycle: 5 of each mesh", r["ok"] and sorted(pm.values()) == [5, 5], r)
r = dress(center=[0, 0, 0], pattern="line", meshes=[CUBE, SPHERE], weights=[1, 0], count=10, to=[900, 0, 0], name="EW_Weights")
pm = (r.get("data") or {}).get("perMesh", {})
check("weights: weight 0 is never picked (10 cubes, 0 spheres)", r["ok"] and pm.get(CUBE) == 10 and pm.get(SPHERE) == 0, r)

# --- ground snap, align base, no ground
r = dress(center=[0, 0, 0], pattern="grid", meshes=[CUBE], rows=2, cols=2, spacing=[300, 300], scale=0.5, snapToGround=True, align="base", name="EW_Ground", output="actors")
d = r.get("data") or {}
t = c("world_trace", {"start": [0, 0, 500], "end": [0, 0, -500]})
top = t["data"]["point"][2]
bot = c("actor_bounds", {"actor": "EW_Ground_001"})["data"]
bot_z = bot["center"][2] - bot["size"][2] / 2
check("snapToGround + align base: cube bottom sits on the floor top", r["ok"] and abs(bot_z - top) < 1.5, (bot_z, top))
r = dress(center=[0, 0, 2000], pattern="grid", meshes=[CUBE], rows=2, cols=2, spacing=[300, 300], snapToGround=True, traceUp=10, traceDown=5, name="EW_NoGround")
check("snapToGround with no ground in reach: NOTHING_PLACED", not r["ok"] and r["errorCode"] == "NOTHING_PLACED", r)
n_before = level_actors()
r = dress(center=[30000, 30000, 0], pattern="line", meshes=[CUBE], count=4, to=[100, 0, 0], snapToGround=True, name="EW_NoGround2")
check("no ground anywhere: NOTHING_PLACED and no actor added", not r["ok"] and r["errorCode"] == "NOTHING_PLACED" and level_actors() == n_before, r)
r = dress(center=[0, 0, 0], pattern="line", meshes=[CUBE], count=10, to=[6000, 0, 0], snapToGround=True, name="EW_PartGround")
d = r.get("data") or {}
check("partly over the floor: placed + skipped add up, warning given", r["ok"] and d["placed"] + d["skipped"]["noGround"] == 10 and d["skipped"]["noGround"] > 0 and r.get("warnings"), r)

# --- overlap and avoid
r = dress(center=[0, 0, 5000], pattern="scatter", meshes=[CUBE], count=60, radius=300, seed=3, avoidOverlap=True, name="EW_Overlap")
d = r.get("data") or {}
check("avoidOverlap on a crowded scatter skips some", r["ok"] and d["skipped"]["overlap"] > 0 and d["placed"] + d["skipped"]["overlap"] == 60, r)
r = dress(center=[0, 0, 5000], pattern="grid", meshes=[CUBE], rows=1, cols=5, spacing=[300, 300], avoidOverlap=True, name="EW_OverlapGrid")
check("avoidOverlap leaves a spread-out grid alone (cubes 100 wide, 300 apart)", r["ok"] and r["data"]["placed"] == 5, r)
r = dress(center=[0, 0, 5000], pattern="grid", meshes=[CUBE], rows=1, cols=5, spacing=[300, 300], avoid=[CENTER], name="EW_Avoid")
check("avoid: grid far from the avoided actor is untouched", r["ok"] and r["data"]["skipped"]["avoid"] == 0, r)
r = dress(center=[1000, 2000, 5000], pattern="grid", meshes=[CUBE], rows=2, cols=2, spacing=[100, 100], avoid=[CENTER], name="EW_Avoid2")
check("avoid: a grid over the avoided actor is skipped (NOTHING_PLACED)", not r["ok"] and r["errorCode"] == "NOTHING_PLACED", r)

# --- jitter changes the result, scale applies
a = dress(center=[0, 0, 0], pattern="grid", meshes=[CUBE], rows=2, cols=2, spacing=[300, 300], scale=[2, 2, 2], scaleJitter=0.3, rotationJitter=[0, 45, 0], seed=5, name="EW_Jit", output="none", export={"table": TABLE, "scene": "J", "prefix": "jit", "save": False})
rows = c("datatable_get", {"path": TABLE})["data"]["rows"]
sx = [rows[k]["Scale"]["x"] for k in rows if k.startswith("jit_")]
check("scaleJitter 0.3 around scale 2: all in 1.4 to 2.6, not all equal", len(sx) == 4 and all(1.39 <= s <= 2.61 for s in sx) and len(set(round(s, 3) for s in sx)) > 1, sx)

# --- export to the DataTable: relative to the target, rows and read back
r = dress(target=CENTER, pattern="ring", meshes=[CUBE, SPHERE], pick="cycle", count=4, radius=200, yaw=0, name="EW_Exp", output="none",
          export={"table": TABLE, "scene": "Deck", "prefix": "Deck", "fields": {"bCollide": True}, "save": False})
ex = (r.get("data") or {}).get("export", {})
check("export: 4 rows written, sample returned", r["ok"] and ex.get("rowsWritten") == 4 and len(ex.get("sample", [])) == 3, r)
rows = c("datatable_get", {"path": TABLE})["data"]["rows"]
deck = {k: v for k, v in rows.items() if k.startswith("Deck_")}


def vec(v):
    return [v["x"], v["y"], v["z"]]


# Target is at (1000,2000,300) with yaw 90: ring point 0 is at +X of the target's frame, i.e. local (200,0,0).
first = deck.get("Deck_001", {})
check("export: rows relative to the target (local 200,0,0 for the first point)", abs(vec(first.get("Location"))[0] - 200) < 0.5 and abs(vec(first.get("Location"))[1]) < 0.5, first)
check("export: Scene, Mesh and fixed fields filled", first.get("Scene") == "Deck" and "Cube" in str(first.get("Mesh")) and first.get("bCollide") is True, first)
check("export: second row has the other mesh (cycle)", "Sphere" in str(deck.get("Deck_002", {}).get("Mesh")), deck.get("Deck_002"))
yaw2 = deck["Deck_002"]["Rotation"]["yaw"]
check("export: facing outward gives the local yaw of the point (90 for the 2nd of 4)", abs(yaw2 - 90) < 0.5, deck.get("Deck_002"))
r = dress(target=CENTER, pattern="ring", meshes=[CUBE], count=3, radius=200, name="EW_Exp", output="none", export={"table": TABLE, "scene": "Deck", "prefix": "Deck", "save": False})
rows = c("datatable_get", {"path": TABLE})["data"]["rows"]
check("export mode replace: old Deck_ rows are swapped (3 now)", r["ok"] and r["data"]["export"]["rowsRemoved"] == 4 and len([k for k in rows if k.startswith("Deck_")]) == 3, r)
before = c("datatable_get", {"path": TABLE})["data"]["rows"]
r = dress(target=CENTER, pattern="ring", meshes=[CUBE], count=3, radius=200, name="EW_Exp", output="none", export={"table": TABLE, "scene": "Deck", "prefix": "Deck", "mode": "add", "save": False})
check("export mode add: existing row -> ROW_EXISTS, table restored", not r["ok"] and r["errorCode"] == "ROW_EXISTS" and c("datatable_get", {"path": TABLE})["data"]["rows"] == before, r)
n = level_actors()
r = dress(target=CENTER, pattern="ring", meshes=[CUBE], count=3, radius=200, name="EW_Exp2", export={"table": TABLE, "scene": "Deck", "prefix": "Deck2", "save": False})
check("export together with placing: both happen", r["ok"] and r["data"]["export"]["rowsWritten"] == 3 and level_actors() == n + 1, r)

# --- pitch / roll base rotation (planks standing on their edge: pitch 90 must stay pitch 90)
r = dress(center=[0, 0, 0], pattern="ring", meshes=[CUBE], count=4, radius=300, pitch=90, name="EW_Pitch", output="none", export={"table": TABLE, "scene": "P", "prefix": "pitch", "save": False})
rows = c("datatable_get", {"path": TABLE})["data"]["rows"]
rot = rows["pitch_002"]["Rotation"] if "pitch_002" in rows else {}
check("pitch 90 stays pitch 90 with the ring yaw in the row", r["ok"] and abs(rot.get("pitch", 0) - 90) < 0.01 and abs(rot.get("roll", 1)) < 0.01 and abs(rot.get("yaw", 0) - 90) < 0.01, rot)
r = dress(center=[0, 0, 0], pattern="ring", meshes=[CUBE], count=4, radius=300, pitch=30, roll=10, name="EW_Pitch", output="none", export={"table": TABLE, "scene": "P", "prefix": "pitch2", "save": False})
rows = c("datatable_get", {"path": TABLE})["data"]["rows"]
rot = rows["pitch2_002"]["Rotation"] if "pitch2_002" in rows else {}
check("pitch and roll are the base rotation of every mesh", r["ok"] and abs(rot.get("pitch", 0) - 30) < 0.01 and abs(rot.get("roll", 0) - 10) < 0.01 and abs(rot.get("yaw", 0) - 90) < 0.01, rot)

# --- errors (each changes nothing)
n = level_actors()
rows_n = len(c("datatable_get", {"path": TABLE})["data"]["rows"])
errs = [
    ("no target or center", dict(pattern="ring", meshes=[CUBE], count=3, radius=100), "INVALID_ARGUMENT"),
    ("target and center", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, target=CENTER, center=[0, 0, 0]), "INVALID_ARGUMENT"),
    ("unknown target", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, target="EW_NoSuch"), "ACTOR_NOT_FOUND"),
    ("unknown mesh", dict(pattern="ring", meshes=["/Game/_EWTest/NoMesh"], count=3, radius=100, center=[0, 0, 0]), "ASSET_NOT_FOUND"),
    ("ring without radius", dict(pattern="ring", meshes=[CUBE], count=3, center=[0, 0, 0]), "INVALID_ARGUMENT"),
    ("count 0", dict(pattern="line", meshes=[CUBE], count=0, to=[1, 0, 0], center=[0, 0, 0]), "INVALID_ARGUMENT"),
    ("too many", dict(pattern="scatter", meshes=[CUBE], count=99999, radius=100, center=[0, 0, 0]), "INVALID_ARGUMENT"),
    ("line without to", dict(pattern="line", meshes=[CUBE], count=3, center=[0, 0, 0]), "INVALID_ARGUMENT"),
    ("grid without spacing", dict(pattern="grid", meshes=[CUBE], rows=2, cols=2, center=[0, 0, 0]), "INVALID_ARGUMENT"),
    ("bad center", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0]), "INVALID_ARGUMENT"),
    ("weights length", dict(pattern="ring", meshes=[CUBE], weights=[1, 2], count=3, radius=100, center=[0, 0, 0]), "INVALID_ARGUMENT"),
    ("scaleJitter 2", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0, 0], scaleJitter=2), "INVALID_ARGUMENT"),
    ("unknown avoid actor", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0, 0], avoid=["EW_NoSuch"]), "ACTOR_NOT_FOUND"),
    ("output none without export", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0, 0], output="none"), "INVALID_ARGUMENT"),
    ("export table missing", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0, 0], export={"table": ROOT + "/DT_Nope"}), "TABLE_NOT_FOUND"),
    ("export unknown key", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0, 0], export={"table": TABLE, "tabel": 1}), "INVALID_ARGUMENT"),
    ("export column missing", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0, 0], export={"table": TABLE, "columns": {"mesh": "Mash"}}), "INVALID_ARGUMENT"),
    ("export bad fixed field", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0, 0], export={"table": TABLE, "fields": {"bCollide": "maybe"}}), None),
    ("unknown argument (registry)", dict(pattern="ring", meshes=[CUBE], count=3, radius=100, center=[0, 0, 0], bogus=1), None),
]
for label, args, code in errs:
    r = c("scene_dress", dict(args, name="EW_Err"))
    check(f"error: {label}", not r["ok"] and (code is None or r["errorCode"] == code), r)
check("errors changed no actor and no row", level_actors() == n and len(c("datatable_get", {"path": TABLE})["data"]["rows"]) == rows_n, (level_actors(), n))

# --- one undo step (editor undo of the whole call)
n = level_actors()
r = dress(center=[0, 0, 0], pattern="line", meshes=[CUBE], count=20, to=[1900, 0, 0], name="EW_Undo", output="actors")
check("undo test: 20 actors placed", r["ok"] and level_actors() == n + 20, r)
u = c("console_command", {"command": "TRANSACTION UNDO"})
check("one undo removes all 20 actors", u["ok"] and level_actors() == n, (u, level_actors(), n))

# --- actor_preview
fx = c("class_measure", {"class": FIXTURE})
have_fx = fx["ok"]
check("actor_preview fixture class found (copy tests/fixtures/EWTestDressActor.* into the test project)", have_fx, fx)
before = level_actors()
if have_fx:
    out1 = os.path.join(os.environ["TEMP"] if "TEMP" in os.environ else ".", "ew_preview_a.png")
    r = c("actor_preview", {"class": FIXTURE, "file": out1, "inline": False, "width": 640, "height": 360, "ground": True})
    d = r.get("data") or {}
    check("actor_preview isolated: PNG written at the requested size", r["ok"] and ewpng.png_size(out1) == (640, 360), r)
    check("actor_preview: BeginPlay ran (ring of 6 cubes widens the bounds to about 640)", r["ok"] and d.get("beganPlay") and d["boundsSize"][0] > 600, d)
    check("actor_preview: image is not empty and not black", r["ok"] and d.get("drawnPercent", 0) > 50, d)
    r0 = c("actor_preview", {"class": FIXTURE, "beginPlay": False, "inline": False, "width": 320, "height": 180})
    d0 = r0.get("data") or {}
    check("actor_preview beginPlay false: only the constructor's cube (about 100)", r0["ok"] and 90 < d0["boundsSize"][0] < 110, d0)
    r3 = c("actor_preview", {"class": FIXTURE, "properties": {"RingCount": 3}, "inline": False, "width": 320, "height": 180})
    check("actor_preview properties are set before BeginPlay", r3["ok"] and r3["data"]["actorsBuilt"] >= 1, r3)
    ra = c("actor_preview", {"class": FIXTURE, "inline": False, "width": 320, "height": 180, "view": "top"})
    rb = c("actor_preview", {"class": FIXTURE, "inline": False, "width": 320, "height": 180, "view": "front"})
    check("actor_preview views differ (top looks down, front looks level)", ra["ok"] and rb["ok"] and ra["data"]["cameraRotation"][0] < -80 and rb["data"]["cameraRotation"][0] > -20, (ra.get("data"), rb.get("data")))
    rc = c("actor_preview", {"class": FIXTURE, "inline": False, "width": 320, "height": 180, "cameraLocation": [-900, 0, 400], "lookAt": [0, 0, 0], "fov": 70})
    check("actor_preview explicit camera and fov are used", rc["ok"] and rc["data"]["cameraLocation"] == [-900, 0, 400] and rc["data"]["fov"] == 70, rc)
    rl = c("actor_preview", {"class": FIXTURE, "world": "level", "inline": False, "width": 320, "height": 180, "location": [0, 0, 5000]})
    check("actor_preview world level works and says so", rl["ok"] and rl["data"]["world"] == "level" and rl["data"]["boundsSize"][0] > 600, rl)
    rl2 = c("actor_preview", {"class": FIXTURE, "world": "level", "inline": False, "width": 320, "height": 180, "cameraActor": CENTER})
    check("actor_preview cameraActor uses that actor's transform", rl2["ok"] and abs(rl2["data"]["cameraLocation"][0] - 1000) < 1 and abs(rl2["data"]["cameraRotation"][1] - 90) < 1, rl2)
    rn = call_raw("actor_preview", {"class": FIXTURE, "width": 320, "height": 180})
    check("actor_preview returns the image in the reply by default", any(b.get("type") == "image" for b in rn.get("content", [])), [b.get("type") for b in rn.get("content", [])])
    check("actor_preview leaves no actor behind", level_actors() == before, (level_actors(), before))
    try:
        os.remove(out1)
    except OSError:
        pass

r = c("actor_preview", {"class": "BP_NoSuchClass"})
check("actor_preview unknown class -> UNKNOWN_CLASS", not r["ok"] and r["errorCode"] == "UNKNOWN_CLASS", r)
r = c("actor_preview", {"class": "StaticMeshComponent"})
check("actor_preview non-actor class -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("actor_preview", {"class": "StaticMeshActor", "width": 8})
check("actor_preview tiny width -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("actor_preview", {"class": "StaticMeshActor", "cameraLocation": [0, 0, 0]})
check("actor_preview cameraLocation without lookAt -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("actor_preview", {"class": "StaticMeshActor", "cameraActor": "EW_NoSuch"})
check("actor_preview unknown cameraActor -> ACTOR_NOT_FOUND", not r["ok"] and r["errorCode"] == "ACTOR_NOT_FOUND", r)
r = c("actor_preview", {"class": "StaticMeshActor", "properties": {"NoSuchProp": 1}, "inline": False})
check("actor_preview bad property -> error, nothing left", not r["ok"] and level_actors() == before, r)
r = c("actor_preview", {"class": "PointLight", "inline": False, "width": 320, "height": 180})
check("actor_preview of an actor without meshes warns, still answers", r["ok"] and r.get("warnings"), r)

cleanup()
check("cleanup: level back to the start", level_actors() == start_actors, (level_actors(), start_actors))
done()
