"""UMG tests: widget_create / tree / add / set / get / remove / render on a fresh Widget Blueprint.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. Writes only under /Game/_EWTest.
"""
import base64, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, call_raw, check, delete_if_exists, done, ensure_stopped

ensure_stopped()
W = ROOT + "/WBP_EWMenu"
delete_if_exists(W)


def tree():
    return {x["name"]: x for x in c("widget_tree", {"path": W})["data"]["widgets"]}


r = c("widget_create", {"path": W})
check("widget_create with CanvasPanel root", r["ok"] and r["data"]["root"] and r["data"]["saved"], r)
root = r["data"]["root"] if r["ok"] else ""
r = c("widget_create", {"path": W})
check("widget_create existing -> ASSET_EXISTS", not r["ok"] and r["errorCode"] == "ASSET_EXISTS", r)
r = c("widget_create", {"path": ROOT + "/WBP_EWBad", "root": "TextBlock"})
check("root that is not a panel -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)

r = c("widget_add", {"path": W, "class": "VerticalBox", "name": "Menu",
                     "slot": {"position": [100, 50], "size": [400, 300], "anchors": [0.5, 0.5, 0.5, 0.5], "alignment": [0.5, 0.5]}})
s = (r.get("data") or {}).get("slot", {})
check("add VerticalBox under root with canvas slot, read back", r["ok"] and r["data"]["parent"] == root and s.get("position") == [100, 50]
      and s.get("size") == [400, 300] and s.get("anchors") == [0.5, 0.5, 0.5, 0.5] and s.get("alignment") == [0.5, 0.5], r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "Title", "parent": "Menu",
                     "properties": {"Text": "Main Menu"}, "slot": {"HorizontalAlignment": "HAlign_Center"}})
check("add TextBlock with text + box slot alignment", r["ok"] and r["data"]["text"] == "Main Menu"
      and r["data"]["slot"]["values"]["HorizontalAlignment"] in ("HAlign_Center", "Center"), r)
r = c("widget_add", {"path": W, "class": "Button", "name": "PlayButton", "parent": "Menu"})
check("add Button", r["ok"] and r["data"]["index"] == 1, r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "PlayLabel", "parent": "PlayButton", "properties": {"Text": "Play"}})
check("add label inside the button", r["ok"] and r["data"]["parent"] == "PlayButton", r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "Second", "parent": "PlayButton"})
check("second child in a Button -> PANEL_FULL", not r["ok"] and r["errorCode"] == "PANEL_FULL", r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "X", "parent": "Title"})
check("child under a TextBlock -> NOT_A_PANEL", not r["ok"] and r["errorCode"] == "NOT_A_PANEL", r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "Title", "parent": "Menu"})
check("duplicate name -> WIDGET_EXISTS", not r["ok"] and r["errorCode"] == "WIDGET_EXISTS", r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "Bad", "parent": "Menu", "properties": {"Txt": "x"}})
check("bad property -> error with suggestion, widget not added", not r["ok"] and "Text" in r.get("suggestions", []) and "Bad" not in tree(), r)
r = c("widget_add", {"path": W, "class": "StaticMeshComponent", "name": "Y"})
check("non-widget class -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "Z", "parent": "Menuu"})
check("unknown parent -> WIDGET_NOT_FOUND with suggestion", not r["ok"] and r["errorCode"] == "WIDGET_NOT_FOUND" and "Menu" in r.get("suggestions", []), r)

r = c("widget_set", {"path": W, "name": "Title", "properties": {"Text": "Changed"}, "slot": {"NoSuchSlotField": 1}})
check("widget_set with a bad slot key -> error, text unchanged", not r["ok"] and tree()["Title"]["text"] == "Main Menu", r)
r = c("widget_set", {"path": W, "name": "Menu", "slot": {"position": [10, 20]}, "properties": {"Visibility": "Collapsed"}})
check("widget_set slot position + visibility, read back", r["ok"] and r["data"]["slot"]["position"] == [10, 20] and r["data"]["visibility"] == "Collapsed", r)
r = c("widget_set", {"path": W, "name": "Menu", "properties": {"Visibility": "Visible"}})
check("widget_set visibility back", r["ok"] and r["data"]["visibility"] == "Visible", r)
r = c("widget_set", {"path": W, "name": root, "slot": {"position": [0, 0]}})
check("slot on the root -> NO_SLOT", not r["ok"] and r["errorCode"] == "NO_SLOT", r)
r = c("widget_get", {"path": W, "name": "Title", "properties": ["Text"]})
check("widget_get reads Text", r["ok"] and "Main Menu" in str(r["data"]["values"]["Text"]), r)

t = tree()
check("widget_tree: 5 widgets with parents", len(t) == 5 and t["PlayLabel"]["parent"] == "PlayButton" and t["Title"]["parent"] == "Menu", list(t))

raw = call_raw("widget_render", {"path": W, "width": 640, "height": 360})
r = raw["structuredContent"]
imgs = [b for b in raw["content"] if b["type"] == "image"]
check("widget_render: PNG, something drawn", r["ok"] and os.path.isfile(r["data"]["file"]) and imgs
      and base64.b64decode(imgs[0]["data"])[:8] == b"\x89PNG\r\n\x1a\n" and r["data"]["drawnPercent"] > 0, r)
r = c("widget_render", {"path": W, "width": 5})
check("widget_render bad size -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)

r = c("widget_remove", {"path": W, "name": "Menu"})
check("widget_remove removes the subtree", r["ok"] and sorted(r["data"]["removed"]) == ["Menu", "PlayButton", "PlayLabel", "Title"] and list(tree()) == [root], r)
r = c("widget_remove", {"path": W, "name": "Menu"})
check("remove missing -> WIDGET_NOT_FOUND", not r["ok"] and r["errorCode"] == "WIDGET_NOT_FOUND", r)
r = c("widget_tree", {"path": ROOT + "/BP_EWTest"})
check("widget_tree on a non-widget Blueprint -> error", not r["ok"], r)

delete_if_exists(W)
done()
