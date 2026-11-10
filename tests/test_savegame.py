"""SaveGame tools (ER-24): savegame_get / savegame_set on a slot.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. The plain-engine checks use a Blueprint SaveGame built with the
generic tools. The checks for a game that wraps its saves (a footer after the engine's bytes) need the C++ fixture
tests/fixtures/EWTestSaveGame.* in the test project's module; without it those checks are skipped with a FAIL line that
says so. The slots are written to <project>/Saved/SaveGames and removed at the end.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, PROJ, call as c, check, delete_if_exists, done, ensure_stopped

ensure_stopped()
BP = ROOT + "/BP_EWSave"
PLAIN, FOOT = "EW_TestPlain", "EW_TestFooter"
FIXTURE = "/Script/EnginewrightTest.EWTestSaveGame"
folder = PROJ / "Saved" / "SaveGames"


def slot_file(name):
    return folder / (name + ".sav")


def cleanup():
    for n in (PLAIN, FOOT):
        try:
            slot_file(n).unlink()
        except FileNotFoundError:
            pass
    delete_if_exists(BP)


cleanup()

# --- a SaveGame Blueprint with three fields
r = c("blueprint_create", {"path": BP, "parent": "SaveGame"})
check("SaveGame Blueprint created", r["ok"], r)
r = c("blueprint_add_variables", {"path": BP, "variables": [
    {"name": "Score", "type": "int", "default": "1"},
    {"name": "PlayerName", "type": "string", "default": "Ragna"},
    {"name": "Done", "type": "bool", "default": "false"}]})
check("SaveGame variables added", r["ok"], r)
cls = BP + ".BP_EWSave_C"

# --- listing and missing slots
r = c("savegame_get", {})
check("savegame_get without slot lists the slots and the folder", r["ok"] and isinstance(r["data"]["slots"], list) and "SaveGames" in r["data"]["folder"], r)
r = c("savegame_get", {"slot": "EW_NoSuchSlot"})
check("savegame_get unknown slot -> SLOT_NOT_FOUND", not r["ok"] and r["errorCode"] == "SLOT_NOT_FOUND", r)
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 1}})
check("savegame_set missing slot without class -> SLOT_NOT_FOUND", not r["ok"] and r["errorCode"] == "SLOT_NOT_FOUND", r)
r = c("savegame_set", {"slot": "../escape", "values": {"Score": 1}, "class": cls})
check("savegame_set path-like slot -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("savegame_set", {"slot": PLAIN, "values": {}, "class": cls})
check("savegame_set empty values -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 1}, "class": "StaticMeshActor"})
check("savegame_set class that is not a SaveGame -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 1}, "class": "NoSuchSaveClass"})
check("savegame_set unknown class -> UNKNOWN_CLASS", not r["ok"] and r["errorCode"] == "UNKNOWN_CLASS", r)
check("a refused call creates no file", not slot_file(PLAIN).exists())

# --- create, read, change (plain engine slot)
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 7, "PlayerName": "Odin", "Done": True}, "class": cls})
check("savegame_set creates the slot, saves it and reads it back", r["ok"] and r["data"]["created"] and r["data"]["savedWith"] == "SaveGameToSlot"
      and r["data"]["values"] == {"Score": 7, "PlayerName": "Odin", "Done": True} and slot_file(PLAIN).is_file(), r)
r = c("savegame_get", {"slot": PLAIN})
d = r.get("data") or {}
check("savegame_get returns class, all fields, no custom format", r["ok"] and d["class"].endswith("BP_EWSave_C") and d["values"] == {"Score": 7, "PlayerName": "Odin", "Done": True}
      and d["customFormat"] is False and d["trailerBytes"] == 0 and d["bytes"] == slot_file(PLAIN).stat().st_size, r)
r = c("savegame_get", {"slot": PLAIN, "fields": ["Score"]})
check("savegame_get fields returns only those", r["ok"] and r["data"]["values"] == {"Score": 7}, r)
r = c("savegame_get", {"slot": PLAIN, "fields": ["Scor"]})
check("savegame_get unknown field -> UNKNOWN_FIELD with suggestion", not r["ok"] and r["errorCode"] == "UNKNOWN_FIELD" and "Score" in r.get("suggestions", []), r)
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 9}})
check("savegame_set changes one field, others stay", r["ok"] and not r["data"]["created"] and r["data"]["values"] == {"Score": 9}
      and c("savegame_get", {"slot": PLAIN})["data"]["values"] == {"Score": 9, "PlayerName": "Odin", "Done": True}, r)
before = slot_file(PLAIN).read_bytes()
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 5, "Nope": 1}})
check("savegame_set unknown field -> UNKNOWN_PROPERTY, nothing written", not r["ok"] and r["errorCode"] == "UNKNOWN_PROPERTY" and slot_file(PLAIN).read_bytes() == before, r)
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": "many"}})
check("savegame_set wrong type -> error, nothing written", not r["ok"] and slot_file(PLAIN).read_bytes() == before, r)
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 1}, "saveFunction": "NoSuchFunction"})
check("savegame_set unknown saveFunction -> FUNCTION_NOT_FOUND, nothing written", not r["ok"] and r["errorCode"] == "FUNCTION_NOT_FOUND" and slot_file(PLAIN).read_bytes() == before, r)
r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 1}, "saveFunction": "GameplayStatics:SaveGameToSlot"})
check("savegame_set saveFunction with the wrong signature -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] in ("INVALID_ARGUMENT", "UNKNOWN_CLASS", "FUNCTION_NOT_FOUND") and slot_file(PLAIN).read_bytes() == before, r)

# --- PIE refuses writes
if c("pie_start", {"timeout": 90})["ok"]:
    r = c("savegame_set", {"slot": PLAIN, "values": {"Score": 1}})
    check("savegame_set during Play In Editor -> PIE_RUNNING", not r["ok"] and r["errorCode"] == "PIE_RUNNING", r)
    r = c("savegame_get", {"slot": PLAIN, "fields": ["Score"]})
    check("savegame_get works during Play In Editor", r["ok"] and r["data"]["values"]["Score"] == 9, r)
    c("pie_stop")

# --- a slot with a game footer (needs the C++ fixture)
probe = c("savegame_set", {"slot": FOOT, "values": {"Value": 1}, "class": FIXTURE})
if not probe["ok"] and probe.get("errorCode") == "UNKNOWN_CLASS":
    check("fixture class " + FIXTURE + " is in the test project (checks for a game footer skipped; see tests/fixtures/EWTestSaveGame.h)", False, probe)
else:
    check("fixture slot created through the engine save", probe["ok"] and slot_file(FOOT).is_file(), probe)
    c("savegame_set", {"slot": FOOT, "values": {"SlotName": FOOT}})
    r = c("savegame_set", {"slot": FOOT, "values": {"Value": 20}, "saveFunction": "SaveWithFooter"})
    raw = slot_file(FOOT).read_bytes()
    check("the game's own save function (on the object) writes the footer, read back from disk", r["ok"] and r["data"]["savedWith"] == "SaveWithFooter"
          and r["data"]["values"] == {"Value": 20} and struct.unpack("<I", raw[-4:])[0] == 20 * 31 + 7, [r, raw[-8:].hex()])
    r = c("savegame_get", {"slot": FOOT})
    check("savegame_get reports the footer as customFormat with its size", r["ok"] and r["data"]["customFormat"] is True and r["data"]["trailerBytes"] == 4
          and r.get("warnings") and r["data"]["values"]["Value"] == 20, r)
    before = slot_file(FOOT).read_bytes()
    r = c("savegame_set", {"slot": FOOT, "values": {"Value": 99}})
    check("savegame_set on a footer slot -> CUSTOM_SAVE_FORMAT, file untouched", not r["ok"] and r["errorCode"] == "CUSTOM_SAVE_FORMAT" and slot_file(FOOT).read_bytes() == before, r)
    r = c("savegame_set", {"slot": FOOT, "values": {"Value": 30}, "saveFunction": FIXTURE + ":SaveStatic"})
    raw = slot_file(FOOT).read_bytes()
    check("a static save function Class:Function works the same", r["ok"] and struct.unpack("<I", raw[-4:])[0] == 30 * 31 + 7 and r["data"]["values"] == {"Value": 30}, [r, raw[-8:].hex()])
    r = c("savegame_set", {"slot": FOOT, "values": {"SlotName": ""}, "saveFunction": "SaveWithFooter"})
    after = slot_file(FOOT).read_bytes()
    check("a save function that fails -> SAVE_FAILED and the old file is restored", not r["ok"] and r["errorCode"] == "SAVE_FAILED" and after == raw, [r, len(after), len(raw)])
    r = c("savegame_set", {"slot": FOOT, "values": {"Value": 1}, "saveFunction": "SaveStatic"})
    check("a static function without Class: -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT" and slot_file(FOOT).read_bytes() == raw, r)

cleanup()
check("test slots and Blueprint removed", not slot_file(PLAIN).exists() and not slot_file(FOOT).exists())
done()
