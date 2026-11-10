"""Core tool tests: assets, DataTables, level actors, settings, log, doctor, batch.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT (any project; fixtures are created by the tests).
"""
import glob, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, PROJ, call as c, check, done, delete_if_exists, fixtures, rpc

T = ROOT
fx = fixtures()
BP = fx["bp"]

# --- cleanup leftovers from earlier runs (only inside /Game/_EWTest)
delete_if_exists(T + "/DT_EW", T + "/BP_EWCopy", T + "/BP_EWMoved", T + "/DT_EWNameCount", T + "/S_EWNameCount",
                 T + "/MAP_EWSaveAs")

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

# --- a failing datatable_set_rows must not corrupt a field named "Name" (JSON round trips are lossy)
NS, NDT = T + "/S_EWNameCount", T + "/DT_EWNameCount"
r = c("struct_create", {"path": NS, "fields": [{"name": "Name", "type": "string"}, {"name": "Count", "type": "int"}]})
check("struct with a Name field created", r["ok"], r)
r = c("datatable_create", {"path": NDT, "rowStruct": NS})
check("datatable for the Name/Count struct created", r["ok"], r)
r = c("datatable_set_rows", {"path": NDT, "rows": {"R1": {"Name": "Keep me", "Count": 1}}})
check("row R1 written", r["ok"], r)
r = c("datatable_set_rows", {"path": NDT, "rows": {"R1": {"Count": 2}, "R2": {"Count": "x"}}})
check("failing set_rows writes nothing", not r["ok"], r)
r = c("datatable_get", {"path": NDT, "rows": ["R1"], "fields": ["Name", "Count"]})
names = c("datatable_get", {"path": NDT, "namesOnly": True})["data"]["rowNames"]
check("a failed set_rows keeps the Name field and does not add the other row",
      r["ok"] and r["data"]["rows"]["R1"]["Name"] == "Keep me" and r["data"]["rows"]["R1"]["Count"] == 1 and "R2" not in names, [r, names])
delete_if_exists(NDT, NS)

# --- rename / delete safety
r = c("asset_delete", {"paths": [T + "/BP_EWCopy"]})
check("delete referenced -> refused with blocking list", not r["ok"] and r["errorCode"] == "ASSET_REFERENCED" and r["data"]["blocking"], r)
rs = r.get("data", {}).get("reasons", [])
check("ER-14: the refusal names a reason per asset (which asset references it, with its class)",
      len(rs) == 1 and rs[0]["asset"] == T + "/BP_EWCopy" and "referenced by" in rs[0]["reason"] and rs[0]["details"], r)
# a name field that holds the asset name as text: a redirector cannot fix it, the rename must report it
r = c("datatable_set_rows", {"path": T + "/DT_EW", "rows": {"Bread": {"DisplayName": "Bread", "ProcessedItemID": "BP_EWCopy"}}})
check("row with the asset name as an ID", r["ok"], r)
r = c("asset_rename", {"source": T + "/BP_EWCopy", "destination": T + "/BP_EWMoved", "dryRun": True})
check("rename dryRun: lists referencer + text reference, renames nothing", r["ok"] and r["data"]["dryRun"]
      and T + "/DT_EW" in r["data"]["referencersToUpdate"]
      and any(h["asset"] == T + "/DT_EW" and h["value"] == "BP_EWCopy" for h in r["data"]["textReferences"])
      and c("asset_exists", {"path": T + "/BP_EWCopy"})["data"]["exists"], r)
r = c("asset_rename", {"source": T + "/BP_EWCopy", "destination": T + "/BP_EWMoved"})
check("rename reports text it could not fix", r["ok"] and any(h["value"] == "BP_EWCopy" for h in r["data"]["textReferences"]) and r.get("warnings"), r)
check("rename: new exists, old gone", r["ok"] and r["data"]["exists"] and not c("asset_exists", {"path": T + "/BP_EWCopy"})["data"]["exists"], r)
r = c("datatable_remove_rows", {"path": T + "/DT_EW", "rows": ["Bread"]})
check("remove the text-reference row again", r["ok"], r)
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

# --- level_save with a path writes the .umap under Content (not the drive root)
orig = c("editor_state")["editor"]["map"]
r = c("level_open", {"path": "/Engine/Maps/Templates/Template_Default", "discard": True})
check("level_open a test map", r["ok"], r)
r = c("level_save", {"path": T + "/MAP_EWSaveAs"})
save_file = PROJ / "Content/_EWTest/MAP_EWSaveAs.umap"
check("level_save with path writes the .umap under Content and returns map + file",
      r["ok"] and r["data"]["map"] == T + "/MAP_EWSaveAs"
      and r["data"]["file"].replace("\\", "/").endswith("Content/_EWTest/MAP_EWSaveAs.umap") and save_file.is_file(), [r, str(save_file)])
check("level_save with path also reports where the editor is (editorMap) and its dirty flag",
      isinstance(r["data"].get("editorMap"), str) and isinstance(r["data"].get("dirty"), bool), r)
back = orig if orig and orig.startswith(("/Game/", "/Engine/")) else "/Engine/Maps/Templates/Template_Default"
r = c("level_open", {"path": back, "discard": True})
check("reopen the original map", r["ok"], r)
delete_if_exists(T + "/MAP_EWSaveAs")
check("saved test map deleted", not c("asset_exists", {"path": T + "/MAP_EWSaveAs"})["data"]["exists"] and not save_file.exists())

# --- every tool argument has a type and a description (describe shows the schema agents see)
def meta(tool, args):
    s, r = rpc("tools/call", {"name": tool, "arguments": args})
    return r["result"]["structuredContent"] if s == 200 else {"ok": False, "http": s, "body": r}

TOOLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Plugins", "Enginewright", "Source", "Enginewright", "Private", "Tools")
tool_names = set()
for path in glob.glob(os.path.join(TOOLS_DIR, "*.cpp")):
    with open(path, encoding="utf-8", errors="replace") as fh:
        tool_names.update(re.findall(r'R\.Register(?:Async)?\(TEXT\("([a-z_0-9]+)"\)', fh.read()))
undocumented = []
for name in sorted(tool_names):
    d = meta("describe", {"name": name})
    if not d["ok"]:
        undocumented.append(name + " (describe failed)")
        continue
    for arg, prop in d["data"]["inputSchema"].get("properties", {}).items():
        if not prop.get("description") or "type" not in prop:
            undocumented.append(name + "." + arg)
check("every argument of every tool has a type and a description", not undocumented, undocumented)

done()
