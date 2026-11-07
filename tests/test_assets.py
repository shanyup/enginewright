"""Asset tool tests: import (texture, sound, font, mesh), texture/mesh info, materials and instances,
mesh material slots, validation.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. Writes only under /Game/_EWTest; import source files
are generated into a temporary folder (no binary fixtures in the repo).
"""
import math, os, struct, sys, tempfile, wave, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, done, delete_if_exists, ensure_stopped

ensure_stopped()
A = ROOT + "/Assets"
delete_if_exists(A + "/M_EWTint", A + "/MI_EWRed", A + "/M_EWGraph", A + "/T_EWChecker", A + "/S_EWBeep",
                 A + "/SM_EWTri", A + "/F_EWFont")


def write_png(path, w=64, h=64):
    rows = b"".join(b"\x00" + b"".join(bytes((255, 255, 255) if (x // 8 + y // 8) % 2 else (40, 40, 200)) for x in range(w)) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def write_wav(path):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050)
        w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(i * 440 * 2 * math.pi / 22050))) for i in range(22050 // 4)))


def write_obj(path):
    with open(path, "w") as f:
        f.write("v 0 0 0\nv 100 0 0\nv 0 100 0\nv 0 0 100\nvn 0 0 1\nvt 0 0\nvt 1 0\nvt 0 1\n"
                "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/2/1\nf 1/1/1 4/2/1 2/3/1\nf 2/1/1 4/2/1 3/3/1\n")


tmp = tempfile.mkdtemp(prefix="ew_import_")
png, wav, obj = os.path.join(tmp, "T_EWChecker.png"), os.path.join(tmp, "S_EWBeep.wav"), os.path.join(tmp, "SM_EWTri.obj")
write_png(png); write_wav(wav); write_obj(obj)
font = r"C:\Windows\Fonts\arial.ttf" if os.path.isfile(r"C:\Windows\Fonts\arial.ttf") else None

# --- import
r = c("asset_import", {"files": [png, os.path.join(tmp, "missing.png")], "folder": A})
check("import with a missing file -> FILE_NOT_FOUND, nothing imported", not r["ok"] and r["errorCode"] == "FILE_NOT_FOUND"
      and not c("asset_exists", {"path": A + "/T_EWChecker"})["data"]["exists"], r)
r = c("asset_import", {"files": [png, wav], "folder": A})
check("import texture + sound in one call", r["ok"] and all(x["assets"] for x in r["data"]["results"]), r)
r = c("asset_import", {"files": [png], "folder": A})
check("re-import without replace -> ASSET_EXISTS", not r["ok"] and r["errorCode"] == "ASSET_EXISTS", r)
r = c("asset_import", {"files": [png], "folder": A, "replace": True})
check("re-import with replace", r["ok"], r)
r = c("asset_import", {"files": [obj], "folder": A})
check("import OBJ mesh", r["ok"] and r["data"]["results"][0]["assets"], r)
mesh_path = next((p for p in (r["data"]["results"][0]["assets"] if r["ok"] else []) if "SM_EWTri" in p), A + "/SM_EWTri")
if font:
    import shutil
    f2 = os.path.join(tmp, "F_EWFont.ttf"); shutil.copy(font, f2)
    r = c("asset_import", {"files": [f2], "folder": A})
    check("import TTF font while the editor is open", r["ok"] and r["data"]["results"][0]["assets"], r)
r = c("asset_import", {"files": [os.path.join(tmp, "x.docx")], "folder": A})
check("unsupported file type -> error", not r["ok"] and r["errorCode"] in ("FILE_NOT_FOUND", "UNSUPPORTED_FILE"), r)

# --- texture / mesh info
r = c("texture_info", {"path": A + "/T_EWChecker"})
check("texture_info size 64x64", r["ok"] and r["data"]["sourceWidth"] == 64 and r["data"]["sourceHeight"] == 64, r)
r = c("object_set", {"path": A + "/T_EWChecker", "values": {"SRGB": False, "LODGroup": "TEXTUREGROUP_UI"}})
check("texture settings via object_set", r["ok"] and not c("texture_info", {"path": A + "/T_EWChecker"})["data"]["sRGB"], r)
r = c("mesh_info", {"path": mesh_path})
check("mesh_info on imported mesh: triangles + slots", r["ok"] and r["data"]["kind"] == "static" and r["data"]["lods"][0]["triangles"] >= 4 and r["data"]["slots"], r)
r = c("mesh_info", {"path": "/Engine/BasicShapes/Cube"})
check("mesh_info on engine cube: 100 cm", r["ok"] and abs(r["data"]["size"][0] - 100) < 1, r)
r = c("mesh_info", {"path": A + "/T_EWChecker"})
check("mesh_info on a texture -> NOT_A_MESH", not r["ok"] and r["errorCode"] == "NOT_A_MESH", r)

# --- materials
r = c("material_create", {"path": A + "/M_EWTint", "inputs": {
    "BaseColor": {"parameter": "Tint", "value": [0.2, 0.4, 0.8]},
    "Roughness": {"parameter": "Rough", "value": 0.35},
    "Metallic": {"value": 0}}})
check("material_create with parameters wired to inputs", r["ok"] and "Tint" in r["data"]["parameters"] and "BaseColor" in r["data"]["properties"]
      and not r["data"]["compileErrors"], r)
r = c("material_create", {"path": A + "/M_EWBad", "inputs": {"BaseColour": {"value": 1}}})
check("unknown material input -> suggestion, nothing created", not r["ok"] and "BaseColor" in r.get("suggestions", [])
      and not c("asset_exists", {"path": A + "/M_EWBad"})["data"]["exists"], r)
r = c("material_instance_create", {"path": A + "/MI_EWRed", "parent": A + "/M_EWTint", "parameters": {"Tint": [1, 0, 0], "Rough": 0.9}})
check("material instance with checked values", r["ok"] and r["data"]["values"]["Tint"][0] == 1 and abs(r["data"]["values"]["Rough"] - 0.9) < 1e-4, r)
r = c("material_instance_set", {"path": A + "/MI_EWRed", "parameters": {"Tnt": [0, 1, 0]}})
check("unknown MI parameter -> suggestion", not r["ok"] and r["errorCode"] == "UNKNOWN_PARAMETER" and "Tint" in r.get("suggestions", []), r)
r = c("material_instance_set", {"path": A + "/MI_EWRed", "parameters": {"Rough": "high"}})
check("wrong MI value type -> error", not r["ok"] and r["errorCode"] == "INVALID_VALUE", r)
r = c("material_instance_set", {"path": A + "/MI_EWRed", "parameters": {"Tint": [0, 1, 0, 1], "Rough": 0.1}})
info = c("material_info", {"path": A + "/MI_EWRed"})
check("material_instance_set + material_info overridden", r["ok"] and info["data"]["parameters"]["Tint"]["overridden"] and info["data"]["parent"].endswith("M_EWTint"), [r, info])

# graph building with A9 pin handling (single-input node, index pins)
r = c("material_create", {"path": A + "/M_EWGraph"})
check("empty material", r["ok"], r)
tex = c("material_add_expression", {"path": A + "/M_EWGraph", "class": "TextureSampleParameter2D", "name": "Albedo", "values": {"Texture": A + "/T_EWChecker"}})
check("add texture parameter with texture", tex["ok"], tex)
mul = c("material_add_expression", {"path": A + "/M_EWGraph", "class": "Multiply"})
k = c("material_add_expression", {"path": A + "/M_EWGraph", "class": "ScalarParameter", "name": "Brightness", "values": {"DefaultValue": 2}})
check("add Multiply + scalar parameter", mul["ok"] and k["ok"], [mul, k])
r = c("material_add_expression", {"path": A + "/M_EWGraph", "class": "Multiplyy"})
check("unknown expression class -> suggestion", not r["ok"] and "Multiply" in r.get("suggestions", []), r)
r = c("material_add_expression", {"path": A + "/M_EWGraph", "class": "ScalarParameter", "name": "X", "values": {"DefualtValue": 1}})
check("bad node property -> error, no node added", not r["ok"] and not any(e.get("parameter") == "X" for e in c("material_info", {"path": A + "/M_EWGraph"})["data"]["expressions"]), r)
r1 = c("material_connect", {"path": A + "/M_EWGraph", "from": "Albedo", "fromOutput": "RGB", "to": mul["data"]["id"], "toInput": "A"})
r2 = c("material_connect", {"path": A + "/M_EWGraph", "from": "Brightness", "to": mul["data"]["id"], "toInput": "#1"})
r3 = c("material_connect", {"path": A + "/M_EWGraph", "from": mul["data"]["id"], "to": "BaseColor"})
check("connect by name, by #index and to BaseColor", r1["ok"] and r2["ok"] and r3["ok"] and not r3["data"]["compileErrors"], [r1, r2, r3])
r = c("material_connect", {"path": A + "/M_EWGraph", "from": "Albedo", "fromOutput": "Purple", "to": "BaseColor"})
check("unknown output pin -> UNKNOWN_PIN lists outputs", not r["ok"] and r["errorCode"] == "UNKNOWN_PIN" and "RGB" in r["message"], r)
info = c("material_info", {"path": A + "/M_EWGraph"})["data"]
check("material_info shows the graph links", info["properties"].get("BaseColor", "").startswith(mul["data"]["id"])
      and any(i.get("from") == tex["data"]["id"] for e in info["expressions"] for i in e["inputs"]), info)

# --- mesh material slot
r = c("mesh_set_material", {"path": mesh_path, "slot": 0, "material": A + "/MI_EWRed"})
check("mesh_set_material by index", r["ok"] and c("mesh_info", {"path": mesh_path})["data"]["slots"][0]["material"].endswith("MI_EWRed"), r)
r = c("mesh_set_material", {"path": mesh_path, "slot": "NoSuchSlot", "material": A + "/MI_EWRed"})
check("unknown slot -> UNKNOWN_SLOT", not r["ok"] and r["errorCode"] == "UNKNOWN_SLOT", r)

# --- validation
r = c("validate", {"folder": ROOT, "checks": ["blueprints", "references", "data"]})
check("validate test folder: blueprints compile, no broken references", r["data"]["blueprints"].get("failed", 0) == 0 and r["data"]["references"]["broken"] == [], r)
r = c("validate", {"checks": ["map"]})
check("validate map runs Map Check", "map" in r["data"], r)
r = c("validate", {"checks": ["typo"]})
check("unknown check name -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)

done()
