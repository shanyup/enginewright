"""Enhanced Input tests: create an action and a mapping context, map keys with modifiers/triggers,
read the bindings back with input_map_list (asset and live player), check on-screen prompts against them
with ui_prompts, then inject the action in Play In Editor and see a Blueprint react to it.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. Writes only under /Game/_EWTest.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, done, delete_if_exists, ensure_stopped

ensure_stopped()
I = ROOT + "/Input"
PROMPT = ROOT + "/WBP_EWPrompt"
DEVSET = "EnhancedInputDeveloperSettings"
delete_if_exists(I + "/IA_EWJump", I + "/IA_EWMove", I + "/IMC_EWTest", PROMPT)

r = c("input_action_create", {"path": I + "/IA_EWJump"})
check("create bool action", r["ok"] and r["data"]["valueType"].endswith("Boolean"), r)
r = c("input_action_create", {"path": I + "/IA_EWMove", "valueType": "axis2d"})
check("create 2D axis action", r["ok"] and r["data"]["valueType"].endswith("Axis2D"), r)
r = c("input_action_create", {"path": I + "/IA_EWJump"})
check("existing action -> ASSET_EXISTS", not r["ok"] and r["errorCode"] == "ASSET_EXISTS", r)
r = c("input_context_create", {"path": I + "/IMC_EWTest"})
check("create mapping context", r["ok"], r)

r = c("input_map", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWJump", "key": "SpaceBar", "triggers": ["Pressed"]})
check("map SpaceBar -> Jump with Pressed trigger", r["ok"] and r["data"]["triggers"] == ["Pressed"], r)
r = c("input_map", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWMove", "key": "S",
                    "modifiers": ["Negate", {"type": "SwizzleAxis", "values": {"Order": "YXZ"}}]})
check("map S -> Move with Negate + SwizzleAxis(YXZ)", r["ok"] and r["data"]["modifiers"] == ["Negate", "SwizzleAxis"], r)
r = c("input_map", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWMove", "key": "D", "modifiers": [{"type": "SwizzleAxis", "values": {"Ordr": "YXZ"}}]})
info = c("input_context_info", {"context": I + "/IMC_EWTest"})["data"]["mappings"]
check("bad modifier property -> error, no mapping added", not r["ok"] and not any(m["key"] == "D" for m in info), r)
r = c("input_map", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWMove", "key": "D", "modifiers": ["Negat"]})
check("unknown modifier type -> suggestion", not r["ok"] and r["errorCode"] == "UNKNOWN_TYPE" and "Negate" in r.get("suggestions", []), r)
r = c("input_map", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWJump", "key": "Spacebar2"})
check("unknown key -> UNKNOWN_KEY", not r["ok"] and r["errorCode"] == "UNKNOWN_KEY", r)
r = c("input_map", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWJump", "key": "SpaceBar"})
check("duplicate mapping -> ALREADY_MAPPED", not r["ok"] and r["errorCode"] == "ALREADY_MAPPED", r)
r = c("input_map", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWJump", "key": "Gamepad_FaceButton_Bottom"})
info = c("input_context_info", {"context": I + "/IMC_EWTest"})["data"]["mappings"]
check("context lists 3 mappings", r["ok"] and len(info) == 3, info)
r = c("input_unmap", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWJump", "key": "Gamepad_FaceButton_Bottom"})
check("unmap one key", r["ok"] and r["data"]["removed"] == 1, r)
r = c("input_unmap", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWJump", "key": "Gamepad_FaceButton_Bottom"})
check("unmap a key that is not mapped -> NOT_MAPPED", not r["ok"] and r["errorCode"] == "NOT_MAPPED", r)

# --- input_map_list (ER-27): which key is bound to which action, per device
r = c("input_map", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWJump", "key": "Gamepad_FaceButton_Right"})
check("map the gamepad B button to Jump", r["ok"], r)
r = c("input_map_list", {"context": I + "/IMC_EWTest"})
d = r.get("data") or {}
acts = {a["name"]: a for a in d.get("actions", [])}
check("input_map_list (asset): source, one context, actions grouped", r["ok"] and d["source"] == "asset" and d["contexts"][0]["context"].endswith("IMC_EWTest")
      and set(acts) == {"IA_EWJump", "IA_EWMove"} and d["actionCount"] == 2 and d["keyCount"] == 3, r)
check("input_map_list: keys per device", acts["IA_EWJump"]["byDevice"] == {"keyboardMouse": ["SpaceBar"], "gamepad": ["Gamepad_FaceButton_Right"]}
      and acts["IA_EWMove"]["byDevice"] == {"keyboardMouse": ["S"]}, [acts["IA_EWJump"]["byDevice"], acts["IA_EWMove"]["byDevice"]])
jk = {k["key"]: k for k in acts["IA_EWJump"]["keys"]}
check("input_map_list: key has display name, device, triggers, modifiers", jk["SpaceBar"]["device"] == "keyboardMouse" and jk["SpaceBar"]["triggers"] == ["Pressed"]
      and jk["SpaceBar"]["display"] and jk["Gamepad_FaceButton_Right"]["device"] == "gamepad" and "modifiers" in jk["SpaceBar"], jk)
check("input_map_list: valueType per action", acts["IA_EWMove"]["valueType"].endswith("Axis2D") and acts["IA_EWJump"]["valueType"].endswith("Boolean"), acts)
r = c("input_map_list", {"context": I + "/IMC_EWTest", "device": "gamepad"})
check("input_map_list device filter: only gamepad keys", r["ok"] and [a["name"] for a in r["data"]["actions"]] == ["IA_EWJump"] and r["data"]["keyCount"] == 1, r)
r = c("input_map_list", {"context": I + "/IMC_EWTest", "action": "*Move*"})
check("input_map_list action filter with a wildcard", r["ok"] and [a["name"] for a in r["data"]["actions"]] == ["IA_EWMove"], r)
r = c("input_map_list", {"context": I + "/IMC_EWTest", "action": I + "/IA_EWJump"})
check("input_map_list action filter by asset path", r["ok"] and [a["name"] for a in r["data"]["actions"]] == ["IA_EWJump"], r)
r = c("input_map_list", {"context": I + "/IMC_EWTest", "action": "IA_Nothing"})
check("input_map_list: nothing matches -> ok, empty, warning", r["ok"] and r["data"]["actionCount"] == 0 and r.get("warnings"), r)
r = c("input_map_list", {"context": I + "/IMC_Missing"})
check("input_map_list missing context -> CONTEXT_NOT_FOUND", not r["ok"] and r["errorCode"] == "CONTEXT_NOT_FOUND", r)
r = c("input_map_list", {"context": I + "/IMC_EWTest", "device": "paddle"})
check("input_map_list bad device -> error", not r["ok"] and r["errorCode"] in ("INVALID_ARGUMENT", "SCHEMA", "INVALID_VALUE"), r)
r = c("input_map_list")
check("input_map_list without context and without PIE -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)
r = c("ui_prompts")
check("ui_prompts without PIE -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)

# --- live player: apply the context through the Enhanced Input developer settings (put back at the end), a screen with hints
prev = c("settings_get", {"class": DEVSET})
prev_ctx = (prev.get("data") or {}).get("values", {}).get("DefaultMappingContexts") if prev["ok"] else None
r = c("settings_set", {"class": DEVSET, "values": {"DefaultMappingContexts": [{"InputMappingContext": I + "/IMC_EWTest.IMC_EWTest", "Priority": 3}]}})
check("settings_set: IMC_EWTest as a default mapping context of the player", r["ok"], r)
made = c("widget_create", {"path": PROMPT, "root": "VerticalBox"})
for name, text in (("HintSpace", "Space"), ("HintB", "B"), ("HintS", "S")):
    c("widget_add", {"path": PROMPT, "class": "TextBlock", "name": name, "properties": {"Text": text}})
check("prompt screen with three hint texts (Space, B, S)", made["ok"], made)
r = c("pie_start", {"timeout": 90})
check("pie_start (with the default mapping context)", r["ok"], r)
r = c("ui_open", {"class": PROMPT})
check("ui_open the prompt screen", r["ok"] and r["data"]["inViewport"], r)
r = c("input_map_list")
d = r.get("data") or {}
acts = {a["name"]: a for a in d.get("actions", [])}
check("input_map_list (player): live source, the applied context with its priority", r["ok"] and d["source"] == "player"
      and any(x["context"].endswith("IMC_EWTest") and x["priority"] == 3 for x in d["contexts"]), r)
check("input_map_list (player): the live mappings of the context", "IA_EWJump" in acts and "SpaceBar" in acts["IA_EWJump"]["byDevice"].get("keyboardMouse", [])
      and "IA_EWMove" in acts, d.get("actions"))
r = c("input_map_list", {"action": "IA_EWJump", "device": "keyboardMouse"})
check("input_map_list (player) filters too", r["ok"] and [a["name"] for a in r["data"]["actions"]] == ["IA_EWJump"] and r["data"]["keyCount"] == 1, r)
r = c("input_map_list", {"player": 5})
check("input_map_list (player) unknown player -> NO_PLAYER", not r["ok"] and r["errorCode"] == "NO_PLAYER", r)

r = c("ui_prompts", {"device": "keyboardMouse", "check": [
    {"prompt": "Space", "action": "IA_EWJump"},      # shown, and Space is bound -> OK
    {"prompt": "B", "action": "IA_EWJump"},          # shown, but B is not a keyboard key of Jump -> MISMATCH
    {"prompt": "Q", "action": "IA_EWJump"},          # not on screen
    {"prompt": "S", "action": "IA_EWMove"},          # shown and bound
    {"prompt": "S", "action": "IA_EWNope"}]})        # no such bound action
d = r.get("data") or {}
st = [x["status"] for x in d.get("checks", [])]
check("ui_prompts: OK / MISMATCH / PROMPT_NOT_SHOWN / OK / ACTION_NOT_BOUND", r["ok"] and st == ["OK", "MISMATCH", "PROMPT_NOT_SHOWN", "OK", "ACTION_NOT_BOUND"], [st, d])
check("ui_prompts: mismatch names the keys the action has and what the shown key triggers", d["checks"][1]["actionKeys"] == ["SpaceBar"] and d["checks"][1]["promptKeyTriggers"] == []
      and d["checks"][0]["shown"] is True and d["checks"][2]["shown"] is False, d.get("checks"))
check("ui_prompts: problems counted and warned", d["problems"] == 3 and d["allOk"] is False and len(r.get("warnings", [])) >= 3, r)
kt = {x["text"]: x for x in d.get("keyTexts", [])}
check("ui_prompts: keyTexts lists key-like on-screen texts with the actions they trigger", "Space" in kt and kt["Space"]["actions"] == ["IA_EWJump"]
      and "S" in kt and kt["S"]["actions"] == ["IA_EWMove"] and "B" not in kt, d.get("keyTexts"))
r = c("ui_prompts", {"device": "gamepad", "check": [{"prompt": "B", "action": "IA_EWJump"}, {"prompt": "Space", "action": "IA_EWJump"}]})
st = [x["status"] for x in (r.get("data") or {}).get("checks", [])]
check("ui_prompts gamepad: face button right shows as B -> OK; a keyboard hint for a gamepad -> MISMATCH", r["ok"] and st == ["OK", "MISMATCH"], r)
c("ui_close", {"widget": "WBP_EWPrompt"})
c("pie_stop")
r = c("settings_set", {"class": DEVSET, "values": {"DefaultMappingContexts": prev_ctx if isinstance(prev_ctx, list) else []}})
check("settings_set: default mapping contexts put back", r["ok"], r)

# --- action injection in PIE: enable the context on the player and inject the action
r = c("pie_start", {"timeout": 90})
check("pie_start", r["ok"], r)
seq = c("log_read", {"limit": 1})["data"]["lastSeq"]
r = c("pie_input", {"action": I + "/IA_EWJump", "seconds": 0.2})
check("inject a bool action (no mapping needed)", r["ok"] and r["data"]["action"].endswith("IA_EWJump"), r)
r = c("pie_input", {"action": I + "/IA_EWMove", "value": [1, 0], "seconds": 0.2})
check("inject a 2D action value", r["ok"] and "X=1" in r["data"]["value"], r)
r = c("pie_input", {"action": I + "/IA_EWMove", "value": "fast"})
check("bad action value -> error", not r["ok"], r)
r = c("pie_input", {"action": I + "/IA_Missing"})
check("missing action asset -> ACTION_NOT_FOUND", not r["ok"] and r["errorCode"] == "ACTION_NOT_FOUND", r)
c("pie_stop")

if not os.environ.get("EW_KEEP_FIXTURES"):
    delete_if_exists(I + "/IA_EWJump", I + "/IA_EWMove", I + "/IMC_EWTest", PROMPT)
done()
