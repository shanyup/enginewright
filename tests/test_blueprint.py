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

done()
