"""Core tool tests: assets, DataTables, level actors, settings, log, doctor, batch.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT (any project; fixtures are created by the tests).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, done, delete_if_exists, fixtures

T = ROOT
fx = fixtures()
BP = fx["bp"]

# --- cleanup leftovers from earlier runs (only inside /Game/_EWTest)
delete_if_exists(T + "/DT_EW", T + "/BP_EWCopy", T + "/BP_EWMoved")

# --- assets
r = c("asset_find", {"folder": T, "class": "Blueprint"})
check("asset_find by folder+class", r["ok"] and any(a["path"] == BP for a in r["data"]["assets"]), r)
r = c("asset_find", {"class": "NotAClass"})
check("asset_find unknown class -> error", not r["ok"] and r["errorCode"] == "UNKNOWN_CLASS", r)
check("asset_exists true/false", c("asset_exists", {"path": BP})["data"]["exists"] and not c("asset_exists", {"path": T + "/Nope"})["data"]["exists"])
r = c("asset_duplicate", {"source": BP, "destination": T + "/BP_EWCopy"})
check("asset_duplicate", r["ok"] and c("asset_exists", {"path": T + "/BP_EWCopy"})["data"]["exists"], r)

# --- datatable on a Blueprint struct (also gives a referencer for the rename/delete tests)
r = c("datatable_create", {"path": T + "/DT_EW", "rowStruct": fx["struct"]})
check("datatable_create with a Blueprint row struct", r["ok"], r)
r = c("datatable_set_rows", {"path": T + "/DT_EW", "rows": {
    "Apple": {"DisplayName": "Apple", "Category": "RawIngredient", "ItemClass": T + "/BP_EWCopy.BP_EWCopy_C"},
    "Pie": {"DisplayName": "Pie", "Category": "Dish", "ProcessedItemID": "Apple"}}})
check("set_rows upsert 2 rows", r["ok"] and len(r["data"]["addedRows"]) == 2, r)
r = c("datatable_get", {"path": T + "/DT_EW", "rows": ["Apple"], "fields": ["ItemClass", "Category"]})
check("get_rows readback (soft class set, enum by display name)", r["ok"] and "BP_EWCopy" in r["data"]["rows"]["Apple"]["ItemClass"]
      and r["data"]["rows"]["Apple"]["Category"] == "RawIngredient", r)
r = c("datatable_set_rows", {"path": T + "/DT_EW", "rows": {"Bread": {"DisplayName": "Bread"}, "Pie": {"Categry": "Dish"}}})
names = c("datatable_get", {"path": T + "/DT_EW", "namesOnly": True})["data"]["rowNames"]
check("bad field in 2nd row -> error, table restored (Bread not added)", not r["ok"] and "Category" in r.get("suggestions", []) and "Bread" not in names, [r, names])
r = c("datatable_set_rows", {"path": T + "/DT_EW", "rows": {"Pie": {"Category": "Soup"}}})
check("bad enum in row -> error with suggestions", not r["ok"] and r["errorCode"] in ("INVALID_VALUE", "ROW_INVALID"), r)
r = c("datatable_set_rows", {"path": T + "/DT_EW", "mode": "update", "rows": {"Cake": {"DisplayName": "x"}}})
check("mode update + missing row -> error", not r["ok"] and r["errorCode"] == "ROW_NOT_FOUND", r)
r = c("datatable_rename_row", {"path": T + "/DT_EW", "from": "Pie", "to": "ApplePie"})
check("rename_row", r["ok"] and "ApplePie" in c("datatable_get", {"path": T + "/DT_EW", "namesOnly": True})["data"]["rowNames"], r)
r = c("datatable_remove_rows", {"path": T + "/DT_EW", "rows": ["ApplePie", "Nope"]})
check("remove_rows with one missing -> nothing removed", not r["ok"] and "ApplePie" in c("datatable_get", {"path": T + "/DT_EW", "namesOnly": True})["data"]["rowNames"], r)

# --- rename / delete safety
r = c("asset_delete", {"paths": [T + "/BP_EWCopy"]})
check("delete referenced -> refused with blocking list", not r["ok"] and r["errorCode"] == "ASSET_REFERENCED" and r["data"]["blocking"], r)
r = c("asset_rename", {"source": T + "/BP_EWCopy", "destination": T + "/BP_EWMoved"})
check("rename: new exists, old gone", r["ok"] and r["data"]["exists"] and not c("asset_exists", {"path": T + "/BP_EWCopy"})["data"]["exists"], r)
r = c("datatable_get", {"path": T + "/DT_EW", "rows": ["Apple"], "fields": ["ItemClass"]})
check("rename updated the DataTable soft reference", r["ok"] and "BP_EWMoved" in r["data"]["rows"]["Apple"]["ItemClass"], r)

# --- scene (open level becomes dirty; actors are removed again)
r = c("actor_spawn", {"class": "PointLight", "label": "EW_TestLight", "location": [0, 0, 300]})
check("actor_spawn", r["ok"] and r["data"]["label"] == "EW_TestLight", r)
r = c("actor_find", {"label": "EW_Test*"})
check("actor_find by label", r["ok"] and r["data"]["total"] >= 1, r)
r = c("actor_set_transform", {"actor": "EW_TestLight", "location": {"x": 100, "y": 0, "z": 300}})
check("actor_set_transform", r["ok"] and r["data"]["location"][0] == 100, r)
r = c("actor_bounds", {"actor": "EW_TestLight"})
check("actor_bounds", r["ok"] and len(r["data"]["size"]) == 3, r)
r = c("world_trace", {"start": [0, 0, 5000], "end": [0, 0, -5000]})
check("world_trace runs", r["ok"] and "hit" in r["data"], r)
r = c("actor_set_transform", {"actor": "EW_TestLigth", "location": [0, 0, 0]})
check("unknown actor -> suggestion", not r["ok"] and "EW_TestLight" in r.get("suggestions", []), r)
r = c("actor_delete", {"actors": ["EW_TestLight"]})
check("actor_delete", r["ok"] and c("actor_find", {"label": "EW_TestLight"})["data"]["total"] == 0, r)

# --- settings / log / doctor
r = c("settings_get", {"class": "GameMapsSettings", "properties": ["EditorStartupMap"]})
check("settings_get", r["ok"] and "EditorStartupMap" in r["data"]["values"], r)
r = c("settings_get", {"class": "GameMapSettings"})
check("settings_get typo -> suggestions", not r["ok"] and r.get("suggestions"), r)
r = c("settings_set", {"class": "GameMapsSettings", "values": {"EditorStartupMapp": "x"}})
check("settings_set unknown property -> error, nothing written", not r["ok"] and r["errorCode"] == "UNKNOWN_PROPERTY", r)
base_seq = c("log_read", {"limit": 1})["data"]["lastSeq"]
c("console_command", {"command": "log LogTemp Display"})
c("console_command", {"command": "ke * EW_NoSuchEvent"})
r = c("log_read", {"since": base_seq, "limit": 20})
check("log_read since returns only new lines", r["ok"] and all(l["seq"] > base_seq for l in r["data"]["lines"]), r)
r = c("doctor")
check("doctor reports", r["ok"] and "zenSessions" in r["data"] and "unsavedPackages" in r["data"], r)
r = c("log_read", {"limit": "ten"})
check("wrong argument type -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)

# --- batch
before = c("object_get", {"path": BP, "classDefault": True, "properties": ["Weight"]})["data"]["properties"]["Weight"]
r = c("batch", {"steps": [
    {"tool": "object_set", "args": {"path": BP, "classDefault": True, "values": {"Weight": 42}}},
    {"tool": "datatable_set_rows", "args": {"path": T + "/DT_EW", "rows": {"Apple": {"DisplayName": "Changed"}}}},
    {"tool": "object_set", "args": {"path": BP, "classDefault": True, "values": {"NotAField": 1}}}]})
after = c("object_get", {"path": BP, "classDefault": True, "properties": ["Weight"]})["data"]["properties"]["Weight"]
name = c("datatable_get", {"path": T + "/DT_EW", "rows": ["Apple"], "fields": ["DisplayName"]})["data"]["rows"]["Apple"]["DisplayName"]
check("atomic batch: step 3 fails -> steps 1-2 undone", not r["ok"] and r["data"].get("rolledBack") and after == before and name == "Apple", [r, after, name])
r = c("batch", {"steps": [{"tool": "asset_delete", "args": {"paths": [T + "/DT_EW"]}}]})
check("atomic batch refuses non-undoable step", not r["ok"] and r["errorCode"] == "NOT_ROLLBACKABLE", r)
r = c("batch", {"steps": [
    {"tool": "object_set", "args": {"path": BP, "classDefault": True, "values": {"Weight": 7}}},
    {"tool": "datatable_set_rows", "args": {"path": T + "/DT_EW", "rows": {"Apple": {"DisplayName": "Apple 2"}}}}]})
check("batch success saves once at the end", r["ok"] and r["data"]["savedPackages"] == 2, r)
c("object_set", {"path": BP, "classDefault": True, "values": {"Weight": 1.5}})

done()
