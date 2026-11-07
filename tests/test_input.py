"""Enhanced Input tests: create an action and a mapping context, map keys with modifiers/triggers,
then inject the action in Play In Editor and see a Blueprint react to it.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. Writes only under /Game/_EWTest.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, done, delete_if_exists, ensure_stopped

ensure_stopped()
I = ROOT + "/Input"
delete_if_exists(I + "/IA_EWJump", I + "/IA_EWMove", I + "/IMC_EWTest")

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

done()
