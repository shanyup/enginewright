"""Type and Blueprint tool tests: enum/struct creation, Blueprint variables, components, graph nodes, compile.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. Writes only under /Game/_EWTest.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, done, delete_if_exists, fixtures

fx = fixtures()
B = ROOT + "/BP_EWGraph"
delete_if_exists(B, ROOT + "/E_EWBad", ROOT + "/S_EWBad")

# --- types
r = c("type_info", {"name": fx["enum"]})
check("type_info enum lists values in order", r["ok"] and [v["name"] for v in r["data"]["values"]] == ["RawIngredient", "Dish", "Tool"], r)
r = c("type_info", {"name": fx["struct"]})
check("type_info struct lists typed fields", r["ok"] and [f["name"] for f in r["data"]["fields"]][:2] == ["DisplayName", "Category"]
      and r["data"]["fields"][2]["type"] == "softclass:Actor", r)
r = c("type_info", {"name": "Vector"})
check("type_info native struct", r["ok"] and any(f["name"] == "X" for f in r["data"]["fields"]), r)
r = c("enum_create", {"path": fx["enum"], "values": ["A"]})
check("enum_create on an existing asset -> ASSET_EXISTS", not r["ok"] and r["errorCode"] == "ASSET_EXISTS", r)
r = c("enum_create", {"path": ROOT + "/E_EWBad", "values": ["A", "a"]})
check("enum_create duplicate value -> error, nothing created", not r["ok"] and not c("asset_exists", {"path": ROOT + "/E_EWBad"})["data"]["exists"], r)
r = c("struct_create", {"path": ROOT + "/S_EWBad", "fields": [{"name": "A", "type": "flaot"}]})
check("struct_create bad type -> INVALID_TYPE, nothing created", not r["ok"] and r["errorCode"] == "INVALID_TYPE" and not c("asset_exists", {"path": ROOT + "/S_EWBad"})["data"]["exists"], r)
r = c("struct_create", {"path": ROOT + "/S_EWBad", "fields": [{"name": "A", "type": "int", "deflt": "1"}]})
check("struct_create unknown field key -> error", not r["ok"] and r["errorCode"] == "UNKNOWN_ARGUMENT", r)

# --- blueprint create / info
r = c("blueprint_create", {"path": B, "parent": "Actr"})
check("blueprint_create unknown parent -> suggestion", not r["ok"] and r["errorCode"] == "UNKNOWN_CLASS" and "Actor" in r.get("suggestions", []), r)
r = c("blueprint_create", {"path": B, "parent": "Actor"})
check("blueprint_create compiles and saves", r["ok"] and r["data"]["compile"]["status"].startswith("upToDate") and r["data"]["saved"], r)

# --- variables
r = c("blueprint_add_variables", {"path": B, "variables": [
    {"name": "Health", "type": "float", "default": "100", "category": "Stats"},
    {"name": "Labels", "type": "name[]"},
    {"name": "Kind", "type": "enum:" + fx["enum"], "default": "Dish"},
    {"name": "Target", "type": "object:Actor", "instanceEditable": False}]})
check("add 4 variables, read back from class defaults", r["ok"] and r["data"]["defaults"]["Health"] == 100 and r["data"]["defaults"]["Kind"] == "Dish", r)
r = c("blueprint_add_variables", {"path": B, "variables": [{"name": "Speed", "type": "float"}, {"name": "Health", "type": "int"}]})
info = c("blueprint_info", {"path": B})["data"]
check("name clash -> NAME_IN_USE, nothing added", not r["ok"] and r["errorCode"] == "NAME_IN_USE" and not any(v["name"] == "Speed" for v in info["variables"]), r)
r = c("blueprint_add_variables", {"path": B, "variables": [{"name": "Tint", "type": "colour"}]})
check("bad variable type -> INVALID_TYPE", not r["ok"] and r["errorCode"] == "INVALID_TYPE", r)
r = c("blueprint_add_variables", {"path": B, "variables": [{"name": "Kind2", "type": "enum:" + fx["enum"], "default": "Dsh"}]})
check("bad enum default -> INVALID_VALUE with suggestion", not r["ok"] and r["errorCode"] == "INVALID_VALUE" and "Dish" in r.get("suggestions", []), r)
check("blueprint_info: variables with type and category", any(v["name"] == "Health" and v["type"] == "float" and v["category"] == "Stats" for v in info["variables"])
      and any(v["name"] == "Labels" and v["type"] == "name[]" for v in info["variables"]), info)
r = c("object_set", {"path": B, "classDefault": True, "values": {"Health": 75, "Kind": "Tool"}})
check("object_set on new variables (strict enum by display name)", r["ok"], r)
r = c("object_set", {"path": B, "classDefault": True, "values": {"Kind": "Toool"}})
check("bad Blueprint enum value -> suggestion", not r["ok"] and "Tool" in r.get("suggestions", []), r)

# --- components
r = c("blueprint_add_component", {"path": B, "class": "StaticMeshComponent", "name": "Body",
                                  "values": {"StaticMesh": "/Engine/BasicShapes/Cube.Cube"}})
check("add StaticMeshComponent with mesh", r["ok"] and ":Body" in r["data"]["template"] or "Body" in r["data"].get("template", ""), r)
r2 = c("object_get", {"path": r["data"].get("template", "/x"), "properties": ["StaticMesh"]}) if r["ok"] else {"ok": False}
check("component template reads back the mesh", r2["ok"] and "Cube" in str(r2["data"]["properties"]["StaticMesh"]), r2)
r = c("blueprint_add_component", {"path": B, "class": "PointLightComponent", "name": "Lamp", "parent": "Body", "values": {"Intensity": 1234}})
check("add child component", r["ok"], r)
r = c("blueprint_add_component", {"path": B, "class": "PointLightComponent", "name": "Lamp2", "values": {"Intensty": 1}})
info = c("blueprint_info", {"path": B})["data"]
check("bad component property -> error, component not added", not r["ok"] and not any(x["name"] == "Lamp2" for x in info["components"]), r)
check("blueprint_info: Lamp under Body", any(x["name"] == "Lamp" and x["parent"] == "Body" for x in info["components"]), info["components"])
r = c("blueprint_add_component", {"path": B, "class": "Actor", "name": "X"})
check("non-component class -> INVALID_CLASS", not r["ok"] and r["errorCode"] == "INVALID_CLASS", r)
r = c("blueprint_remove_component", {"path": B, "name": "Lamp"})
info = c("blueprint_info", {"path": B})["data"]
check("remove component", r["ok"] and not any(x["name"] == "Lamp" for x in info["components"]), r)

# --- graph: BeginPlay -> PrintString("hello")
r = c("blueprint_add_node", {"path": B, "kind": "event", "name": "BeginPlay", "position": [0, 0]})
check("add BeginPlay event", r["ok"], r)
ev = r["data"]["id"] if r["ok"] else ""
r = c("blueprint_add_node", {"path": B, "kind": "call", "name": "PrintString", "position": [300, 0]})
check("add PrintString call", r["ok"] and any(p["name"] == "InString" for p in r["data"]["pins"]), r)
pr = r["data"]["id"] if r["ok"] else ""
r = c("blueprint_connect", {"path": B, "from": ev + ".then", "to": pr + ".execute"})
check("connect exec pins (aliases)", r["ok"], r)
r = c("blueprint_set_pin", {"path": B, "pin": pr + ".InString", "value": "EW hello"})
check("set pin default", r["ok"] and r["data"]["value"] == "EW hello", r)
r = c("blueprint_add_node", {"path": B, "kind": "get", "name": "Health"})
check("add variable get", r["ok"], r)
gh = r["data"]["id"] if r["ok"] else ""
r = c("blueprint_connect", {"path": B, "from": gh + ".Health", "to": pr + ".InString"})
check("float -> string connection is accepted with a conversion note or refused clearly", r["ok"] or r["errorCode"] == "CONNECTION_REFUSED", r)
r = c("blueprint_connect", {"path": B, "from": ev + ".then", "to": pr + ".Nope"})
check("unknown pin -> UNKNOWN_PIN with pin list", not r["ok"] and r["errorCode"] == "UNKNOWN_PIN", r)
r = c("blueprint_add_node", {"path": B, "kind": "event", "name": "BeginPlay"})
check("second BeginPlay returns the same node (existing)", r["ok"] and r["data"]["existing"] and r["data"]["id"] == ev, r)
r = c("blueprint_add_node", {"path": B, "kind": "call", "name": "PrintStrng"})
check("unknown function -> suggestion", not r["ok"] and r["errorCode"] == "UNKNOWN_FUNCTION" and "PrintString" in r["message"], r)
g = c("blueprint_graph_read", {"path": B})
links = [l for n in g["data"]["nodes"] for p in n["pins"] for l in p.get("links", [])]
check("graph_read shows the link", g["ok"] and any(l.startswith(pr) for l in links), g)
r = c("blueprint_compile", {"paths": [B]})
check("blueprint_compile clean", r["ok"] and r["data"]["failed"] == 0, r)
r = c("blueprint_remove_node", {"path": B, "node": gh})
check("remove node", r["ok"], r)

# --- remove variables
r = c("blueprint_remove_variables", {"path": B, "names": ["Labels", "Nope"]})
check("remove unknown variable -> nothing removed", not r["ok"] and r["errorCode"] == "UNKNOWN_VARIABLE", r)
r = c("blueprint_remove_variables", {"path": B, "names": ["Labels"]})
info = c("blueprint_info", {"path": B})["data"]
check("remove variable", r["ok"] and not any(v["name"] == "Labels" for v in info["variables"]), r)
r = c("blueprint_compile", {"folder": ROOT})
check("compile whole test folder", r["ok"] and r["data"]["compiled"] >= 2, r)

# --- ER-18: defaults of inherited and own components (C++ parent, parent Blueprint, own component)
H, HC = ROOT + "/BP_EWHero", ROOT + "/BP_EWHeroChild"
delete_if_exists(HC, H)
r = c("blueprint_create", {"path": H, "parent": "Character"})
check("blueprint_create with a Character parent", r["ok"], r)
info = c("blueprint_info", {"path": H})["data"]
inh = {x["name"]: x for x in info["inheritedComponents"]}
check("blueprint_info lists the components inherited from the C++ parent (CharacterMesh0 = Mesh, CollisionCylinder = CapsuleComponent, movement)",
      "CharacterMesh0" in inh and inh["CharacterMesh0"].get("property") == "Mesh" and inh["CharacterMesh0"]["class"] == "SkeletalMeshComponent"
      and inh["CharacterMesh0"]["source"] == "native" and "CollisionCylinder" in inh and inh["CollisionCylinder"].get("property") == "CapsuleComponent" and "CharMoveComp" in inh, info["inheritedComponents"])


def mesh_values(bp, comp="CharacterMesh0", names=("RelativeLocation", "RelativeScale3D")):
    return c("object_get", {"path": f"{bp}.Default__{bp.rsplit('/', 1)[1]}_C:{comp}", "properties": list(names)})["data"]["properties"]


before = mesh_values(H)
r = c("blueprint_set_component_defaults", {"path": H, "component": "Mesh", "values": {"RelativeLocation": {"X": 0, "Y": 0, "Z": -77}, "RelativeScale3D": {"X": 2, "Y": 2, "Z": 2}}})
back = (r.get("data") or {}).get("values", {})
check("set the defaults of the inherited Mesh component (by its native property name): read back after the compile, saved",
      r["ok"] and r["data"]["saved"] and back.get("RelativeLocation", {}).get("z") == -77 and back.get("RelativeScale3D", {}).get("x") == 2, r)
now = mesh_values(H)
check("the class default object really holds the new values (object_get on Default__BP_EWHero_C:CharacterMesh0)",
      now["RelativeLocation"]["z"] == -77 and now["RelativeScale3D"]["y"] == 2 and before["RelativeLocation"]["z"] != -77, [before, now])
r = c("blueprint_compile", {"paths": [H]})
check("the values survive another compile (the class default object is rebuilt)", r["ok"] and mesh_values(H)["RelativeLocation"]["z"] == -77, r)
r = c("blueprint_set_component_defaults", {"path": H, "component": "CharacterMesh0", "values": {"RelativeLocation": {"X": 0, "Y": 0, "Z": -60}}})
check("same component by its real name", r["ok"] and mesh_values(H)["RelativeLocation"]["z"] == -60, r)
r = c("blueprint_set_component_defaults", {"path": H, "component": "Meshh", "values": {"RelativeLocation": {"X": 1, "Y": 1, "Z": 1}}})
check("unknown component -> UNKNOWN_COMPONENT with suggestions", not r["ok"] and r["errorCode"] == "UNKNOWN_COMPONENT" and any(x in r.get("suggestions", []) for x in ("Mesh", "CharacterMesh0")), r)
r = c("blueprint_set_component_defaults", {"path": H, "component": "Mesh", "values": {"RelativeLocatio": {"X": 1, "Y": 1, "Z": 1}}})
check("unknown property -> UNKNOWN_PROPERTY, nothing written", not r["ok"] and r["errorCode"] == "UNKNOWN_PROPERTY" and mesh_values(H)["RelativeLocation"]["z"] == -60, r)
r = c("blueprint_set_component_defaults", {"path": H, "component": "Mesh", "values": {"RelativeLocation": {"X": 5, "Y": 5, "Z": 5}, "RelativeScale3D": "big"}})
check("one bad value among good ones -> error, nothing written", not r["ok"] and r["errorCode"] in ("INVALID_VALUE", "WRITE_FAILED") and mesh_values(H)["RelativeLocation"]["x"] == 0, r)
r = c("blueprint_set_component_defaults", {"path": fx["enum"], "component": "Mesh", "values": {"A": 1}})
check("not a Blueprint -> BLUEPRINT_NOT_FOUND", not r["ok"] and r["errorCode"] == "BLUEPRINT_NOT_FOUND", r)
r = c("batch", {"steps": [
    {"tool": "blueprint_set_component_defaults", "args": {"path": H, "component": "Mesh", "values": {"RelativeLocation": {"X": 9, "Y": 9, "Z": 9}}}},
    {"tool": "object_set", "args": {"path": fx["bp"], "classDefault": True, "values": {"NotAField": 1}}}]})
check("one undo step: an atomic batch that fails after the component write rolls it back", not r["ok"] and r["data"].get("rolledBack") and mesh_values(H)["RelativeLocation"]["x"] == 0, [r, mesh_values(H)])

# a component the Blueprint adds itself, and a child Blueprint changing a component of its parent Blueprint
r = c("blueprint_add_component", {"path": H, "class": "StaticMeshComponent", "name": "Body"})
check("blueprint_add_component Body", r["ok"], r)
r = c("blueprint_set_component_defaults", {"path": H, "component": "Body", "values": {"StaticMesh": "/Engine/BasicShapes/Cube.Cube", "RelativeScale3D": {"X": 3, "Y": 3, "Z": 3}}})
tpl = (c("blueprint_info", {"path": H})["data"]["components"] or [{}])[0].get("template", "")
got = c("object_get", {"path": tpl, "properties": ["StaticMesh", "RelativeScale3D"]})["data"]["properties"] if tpl else {}
check("own SCS component Body: StaticMesh and scale written", r["ok"] and r["data"]["source"] == "blueprint" and got.get("RelativeScale3D", {}).get("x") == 3 and "Cube" in str(got.get("StaticMesh")), [r, got])
r = c("blueprint_create", {"path": HC, "parent": H})
check("blueprint_create child of a Blueprint", r["ok"], r)
inh = {x["name"]: x for x in c("blueprint_info", {"path": HC})["data"]["inheritedComponents"]}
check("child lists the parent Blueprint's component and the C++ ones", inh.get("Body", {}).get("source") == "blueprint:BP_EWHero" and "CharacterMesh0" in inh, inh)
r = c("blueprint_set_component_defaults", {"path": HC, "component": "Body", "values": {"RelativeScale3D": {"X": 5, "Y": 5, "Z": 5}}})
child_tpl = (r.get("data") or {}).get("template", "")
child = c("object_get", {"path": child_tpl, "properties": ["RelativeScale3D"]})["data"]["properties"] if child_tpl else {}
parent = c("object_get", {"path": tpl, "properties": ["RelativeScale3D"]})["data"]["properties"]
check("child changes the parent Blueprint's component (override in the child); the parent keeps its value",
      r["ok"] and r["data"]["source"] == "blueprint:BP_EWHero" and child.get("RelativeScale3D", {}).get("x") == 5 and parent["RelativeScale3D"]["x"] == 3, [r, child, parent])
r = c("blueprint_set_component_defaults", {"path": HC, "component": "CharacterMesh0", "values": {"RelativeLocation": {"X": 0, "Y": 0, "Z": -10}}})
check("child changes the C++ component too, without touching the parent Blueprint",
      r["ok"] and mesh_values(HC)["RelativeLocation"]["z"] == -10 and mesh_values(H)["RelativeLocation"]["z"] == -60, [r, mesh_values(HC), mesh_values(H)])
delete_if_exists(HC, H)
check("ER-18 fixtures deleted", not c("asset_exists", {"path": H})["data"]["exists"] and not c("asset_exists", {"path": HC})["data"]["exists"])

done()
