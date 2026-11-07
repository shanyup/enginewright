"""Runtime UI tests (UMG + CommonUI): ui_open / ui_state / ui_click / ui_close in Play In Editor, CommonUI assets
built with the generic tools (text/button style Blueprints, activatable page, CommonButtonBase, CommonTextBlock with a style).

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT with the CommonUI plugin enabled (the CommonUI part is skipped
with a FAIL line that says so when it is not). Writes only under /Game/_EWTest.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, delete_if_exists, done, ensure_stopped

ensure_stopped()
CLICK, PAGE, CBTN, HOST = ROOT + "/WBP_EWClick", ROOT + "/WBP_EWPage", ROOT + "/WBP_EWCButton", ROOT + "/WBP_EWHost"
CTS, CBS = ROOT + "/CTS_EWText", ROOT + "/CBS_EWButton"
AUDIT, IA, DT = ROOT + "/WBP_EWAudit", ROOT + "/IA_EWUiConfirm", ROOT + "/DT_EWUiActions"
delete_if_exists(HOST, PAGE, AUDIT, CBTN, CLICK, CTS, CBS, IA, DT)


def ok(r, label):
    if not r["ok"]:
        raise RuntimeError(f"{label}: {r}")
    return r


# --- plain UMG screen: two buttons, the second one under an image
ok(c("widget_create", {"path": CLICK}), "create click")
ok(c("widget_add", {"path": CLICK, "class": "Button", "name": "OkButton", "slot": {"position": [100, 100], "size": [200, 80]}}), "ok button")
ok(c("widget_add", {"path": CLICK, "class": "TextBlock", "name": "OkLabel", "parent": "OkButton", "properties": {"Text": "OK"}}), "ok label")
ok(c("widget_add", {"path": CLICK, "class": "Button", "name": "UnderButton", "slot": {"position": [400, 100], "size": [200, 80]}}), "under button")
ok(c("widget_add", {"path": CLICK, "class": "TextBlock", "name": "UnderLabel", "parent": "UnderButton", "properties": {"Text": "Under"}}), "under label")
ok(c("widget_add", {"path": CLICK, "class": "Image", "name": "Cover", "slot": {"position": [380, 80], "size": [260, 140]}}), "cover")

# --- CommonUI assets with the generic tools
common = c("blueprint_create", {"path": CTS, "parent": "CommonTextStyle"})
check("CommonUI: text style Blueprint (CommonTextStyle parent)", common["ok"], common)
if common["ok"]:
    r = c("blueprint_create", {"path": CBS, "parent": "CommonButtonStyle"})
    check("CommonUI: button style Blueprint", r["ok"], r)
    r = c("object_set", {"path": CTS, "classDefault": True, "values": {"Color": {"R": 1, "G": 0.5, "B": 0, "A": 1}}})
    check("CommonUI: set the text style color on the class defaults, read back", r["ok"], r)
    r = c("widget_create", {"path": CBTN, "parent": "CommonButtonBase", "root": "Overlay"})
    check("CommonUI: CommonButtonBase Widget Blueprint", r["ok"], r)
    r = c("widget_add", {"path": CBTN, "class": "CommonTextBlock", "name": "Label", "properties": {"Text": "Go", "Style": CTS + ".CTS_EWText_C"}})
    check("CommonUI: CommonTextBlock with a text style", r["ok"], r)
    r = c("widget_create", {"path": PAGE, "parent": "CommonActivatableWidget", "root": "VerticalBox"})
    check("CommonUI: activatable page", r["ok"], r)
    r = c("widget_add", {"path": PAGE, "class": CBTN + ".WBP_EWCButton_C", "name": "GoButton", "properties": {"bSelectable": True, "bToggleable": True}})
    check("CommonUI: CommonButtonBase in a page, made toggleable", r["ok"], r)
    ok(c("widget_add", {"path": PAGE, "class": CBTN + ".WBP_EWCButton_C", "name": "Go2"}), "second CommonButtonBase")
    r = c("object_set", {"path": CBTN, "classDefault": True, "values": {"bSelectable": True}})
    g2 = c("widget_get", {"path": PAGE, "name": "Go2", "properties": ["bSelectable"]})
    check("object_set on class defaults reaches existing templates in other Blueprints", r["ok"] and r["data"].get("instancesUpdated", 0) >= 1
          and g2["ok"] and g2["data"]["values"]["bSelectable"] is True, [r, g2])
    ok(c("widget_add", {"path": PAGE, "class": "Button", "name": "NextButton"}), "next")
    ok(c("widget_add", {"path": PAGE, "class": "TextBlock", "name": "NextLabel", "parent": "NextButton", "properties": {"Text": "Next"}}), "next label")
    r = c("widget_create", {"path": HOST, "root": "Overlay"})
    r2 = c("widget_add", {"path": HOST, "class": "CommonActivatableWidgetStack", "name": "Stack"}) if r["ok"] else r
    check("CommonUI: host with an activatable stack", r2["ok"], r2)

    # --- input wiring audit (separate widget, never opened in PIE)
    ok(c("input_action_create", {"path": IA}), "input action")
    dt = c("datatable_create", {"path": DT, "rowStruct": "CommonInputActionDataBase"})
    check("CommonUI: DataTable of CommonInputActionDataBase rows", dt["ok"], dt)
    ok(c("widget_create", {"path": AUDIT, "root": "VerticalBox"}), "audit widget")
    ok(c("widget_add", {"path": AUDIT, "class": CBTN + ".WBP_EWCButton_C", "name": "Confirm1", "properties": {"TriggeringEnhancedInputAction": IA}}), "confirm1")
    ok(c("widget_add", {"path": AUDIT, "class": CBTN + ".WBP_EWCButton_C", "name": "Confirm2", "properties": {"TriggeringEnhancedInputAction": IA}}), "confirm2")
    r = c("widget_add", {"path": AUDIT, "class": CBTN + ".WBP_EWCButton_C", "name": "RowBtn", "properties": {"TriggeringInputAction": {"DataTable": DT, "RowName": "Confirm"}}})
    check("CommonUI: button with a DataTable input action row", r["ok"], r)
    a = c("ui_input_audit", {"path": ROOT})
    issues = (a.get("data") or {}).get("issues", [])
    got = {(i["code"], i.get("widget")) for i in issues if i["asset"].endswith("WBP_EWAudit")}
    check("ui_input_audit: Enhanced Input action on a button while support is off", a["ok"] and (a["data"]["enhancedInputSupport"] or ("ENHANCED_INPUT_OFF", "Confirm1") in got), issues)
    check("ui_input_audit: two buttons with the same action -> DUPLICATE_ACTION", ("DUPLICATE_ACTION", "Confirm2") in got, issues)
    check("ui_input_audit: missing DataTable row -> ROW_MISSING", ("ROW_MISSING", "RowBtn") in got, issues)
    check("ui_input_audit: reports the CommonUI input data setting", a["ok"] and (a["data"]["inputDataSet"] or any(i["code"] == "NO_INPUT_DATA" for i in issues))
          and a["data"]["commonButtons"] >= 5, a.get("data"))
    r = c("ui_input_audit", {"path": "Game/UI"})
    check("ui_input_audit bad path -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
else:
    check("CommonUI plugin is enabled in the test project (CommonUI checks skipped)", False, common)

# --- Play In Editor
r = c("ui_state")
check("ui_state without PIE -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)
ok(c("pie_start", {"timeout": 90}), "pie_start")
r = c("ui_open", {"class": CLICK})
check("ui_open puts the screen in the viewport", r["ok"] and r["data"]["inViewport"], r)
r = c("ui_state")
btn = {b["label"]: b for b in (r.get("data") or {}).get("buttons", [])}
check("ui_state lists the buttons with labels and rects", r["ok"] and "OK" in btn and len(btn["OK"]["rect"]) == 4
      and btn["OK"]["rect"][2] - btn["OK"]["rect"][0] > 50, r)
check("ui_state lists the screen in the viewport", any(w["class"].startswith("WBP_EWClick") for w in r["data"]["viewportWidgets"]), r["data"]["viewportWidgets"])
r = c("ui_click", {"button": "OK"})
check("ui_click: uncovered button is clicked", r["ok"] and r["data"]["clicked"]["name"] == "OkButton" and r["data"]["clicked"]["method"] == "click", r)
r = c("ui_click", {"button": "Under"})
check("ui_click on a covered button -> COVERED naming the cover", not r["ok"] and r["errorCode"] == "COVERED" and "Cover" in r["message"], r)
r = c("ui_click", {"button": "OK", "method": "tap"})
check("ui_click bad method -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("ui_click", {"button": "Under", "method": "event"})
check("ui_click method event fires it anyway", r["ok"] and r["data"]["clicked"]["method"] == "event", r)
r = c("ui_click", {"button": "Nope"})
check("ui_click unknown -> BUTTON_NOT_FOUND with suggestions", not r["ok"] and r["errorCode"] == "BUTTON_NOT_FOUND" and r.get("suggestions"), r)
r = c("ui_click", {"button": "*"})
check("ui_click matching several -> AMBIGUOUS", not r["ok"] and r["errorCode"] == "AMBIGUOUS", r)
shot = c("screenshot", {"inline": False, "maxWidth": 640})
check("screenshot of the open screen", shot["ok"] and os.path.isfile(shot["data"]["file"]), shot)
r = c("ui_close", {"widget": "WBP_EWClick"})
check("ui_close removes it", r["ok"] and not any(b["label"] == "OK" for b in c("ui_state")["data"]["buttons"]), r)
r = c("ui_close", {"widget": "WBP_EWClick"})
check("ui_close again -> WIDGET_NOT_FOUND", not r["ok"] and r["errorCode"] == "WIDGET_NOT_FOUND", r)

if common["ok"]:
    r = c("ui_open", {"class": PAGE})
    check("CommonUI: ui_open activates an activatable page", r["ok"] and r["data"]["activated"], r)
    r = c("ui_state")
    d = r.get("data") or {}
    check("CommonUI: ui_state reports the activated page", d.get("commonUI") and any(a["class"].startswith("WBP_EWPage") and a["activated"] for a in d.get("activatableWidgets", [])), d)
    labels = {b["label"]: b for b in d.get("buttons", [])}
    check("CommonUI: CommonButtonBase listed once with its label", "Go" in labels and labels["Go"]["class"].startswith("WBP_EWCButton")
          and sum(1 for b in d.get("buttons", []) if b["name"] == "GoButton") == 1, d.get("buttons"))
    r = c("ui_click", {"button": "GoButton"})
    go = [b for b in (r.get("data") or {}).get("buttons", []) if b["name"] == "GoButton"]
    check("CommonUI: click a CommonButtonBase (toggle is now selected)", r["ok"] and r["data"]["clicked"]["name"] == "GoButton" and go and go[0].get("selected") is True, r)
    r = c("ui_click", {"button": "GoButton", "method": "event"})
    go = [b for b in (r.get("data") or {}).get("buttons", []) if b["name"] == "GoButton"]
    check("CommonUI: event click toggles it back", r["ok"] and go and go[0].get("selected") is False, r)
    c("ui_close", {"widget": "WBP_EWPage"})
    r = c("ui_open", {"class": HOST})
    st = (c("ui_state").get("data") or {}).get("stacks", [])
    check("CommonUI: ui_state lists the activatable stack", r["ok"] and any(s["name"] == "Stack" and s["widgets"] == [] for s in st), st)
    r = c("ui_open", {"class": PAGE, "stack": "Stack"})
    check("CommonUI: ui_open with stack pushes and activates the page", r["ok"] and r["data"]["stackSize"] == 1 and r["data"]["activated"], r)
    d = c("ui_state").get("data") or {}
    check("CommonUI: the stack's top is the pushed page", any(s["name"] == "Stack" and s.get("top", "").startswith("WBP_EWPage") for s in d.get("stacks", [])), d.get("stacks"))
    check("CommonUI: pushed page's buttons are listed", any(b["name"] == "GoButton" for b in d.get("buttons", [])), d.get("buttons"))
    r = c("ui_open", {"class": PAGE, "stack": "NoSuchStack"})
    check("ui_open unknown stack -> STACK_NOT_FOUND", not r["ok"] and r["errorCode"] == "STACK_NOT_FOUND", r)
    r = c("ui_open", {"class": CLICK, "stack": "Stack"})
    check("ui_open non-activatable onto a stack -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
    r = c("ui_input_type")
    check("ui_input_type reads the current type", r["ok"] and r["data"]["inputType"] in ("MouseAndKeyboard", "Gamepad", "Touch") and d.get("inputType") == r["data"]["inputType"], [r, d.get("inputType")])
    r = c("ui_input_type", {"type": "Gamepad"})
    check("ui_input_type switches to Gamepad (read back)", r["ok"] and r["data"]["inputType"] == "Gamepad", r)
    check("ui_state reports Gamepad", (c("ui_state").get("data") or {}).get("inputType") == "Gamepad")
    r = c("ui_input_type", {"type": "MouseAndKeyboard"})
    check("ui_input_type back to MouseAndKeyboard", r["ok"] and r["data"]["inputType"] == "MouseAndKeyboard" and r["data"]["before"] == "Gamepad", r)
    r = c("ui_input_type", {"type": "Joystick"})
    check("ui_input_type bad type -> error", not r["ok"] and r["errorCode"] in ("INVALID_ARGUMENT", "SCHEMA"), r)
    r = c("ui_close", {"widget": "WBP_EWPage"})
    check("CommonUI: ui_close removes a page from its stack", r["ok"], r)
    time.sleep(1.0)
    st = (c("ui_state").get("data") or {}).get("stacks", [])
    check("CommonUI: the stack is empty again", any(s["name"] == "Stack" and s["widgets"] == [] for s in st), st)
    c("ui_close", {"widget": "WBP_EWHost"})
r = c("ui_open", {"class": "StaticMeshActor"})
check("ui_open non-widget -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
c("pie_stop")

if not os.environ.get("EW_KEEP_FIXTURES"):
    delete_if_exists(HOST, PAGE, AUDIT, CBTN, CLICK, CTS, CBS, IA, DT)
done()
