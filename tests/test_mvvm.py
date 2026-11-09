"""MVVM tests: viewmodel_list / add / bind / unbind / remove on a fresh Widget Blueprint.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. Two states are handled:
  - MVVM off (the ModelViewViewModel and EnginewrightMVVM plugins are not enabled): every viewmodel_* tool must answer
    MVVM_NOT_ENABLED with the way to turn it on, and the other tools must keep working. Run this suite against a clean
    project (for example a fresh install) to check this state.
  - MVVM on: the full acceptance test. It needs the C++ view model of tests/fixtures/EWTestViewModel.* (copy it into the
    test project's module and add "ModelViewViewModel" and "FieldNotification" to that module's Build.cs); without it
    the on-state checks are skipped.
Writes only under /Game/_EWTest.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, delete_if_exists, done, ensure_stopped, rpc

ensure_stopped()
W = ROOT + "/WBP_EWShop"
VM = "/Script/EnginewrightTest.EWTestViewModel"
TOOLS = ["viewmodel_list", "viewmodel_add", "viewmodel_bind", "viewmodel_unbind", "viewmodel_remove"]

# --- the tools are always there, with a description that says what they need
for t in TOOLS:
    s, r = rpc("tools/call", {"name": "describe", "arguments": {"name": t}})
    d = r["result"]["structuredContent"] if s == 200 else {}
    text = (d.get("data") or {}).get("description", "")
    check(f"{t} is registered and its description names the MVVM plugins", d.get("ok") and "ModelViewViewModel" in text and "MVVM_NOT_ENABLED" in text, d)

probe = c("viewmodel_list", {"widget": W})
if not probe["ok"] and probe.get("errorCode") == "MVVM_NOT_ENABLED":
    # ---------------------------------------------------------------- MVVM off
    msg = probe.get("message", "")
    check("MVVM off: viewmodel_list -> MVVM_NOT_ENABLED that says how to enable it", "ModelViewViewModel" in msg and "EnginewrightMVVM" in msg and "restart" in msg, probe)
    calls = {
        "viewmodel_add": {"widget": W, "viewModel": VM},
        "viewmodel_bind": {"widget": W, "viewModel": "X", "source": "Gold", "target": "Gold.Text"},
        "viewmodel_unbind": {"widget": W, "target": "Gold.Text"},
        "viewmodel_remove": {"widget": W, "viewModel": "X"},
    }
    for t, a in calls.items():
        r = c(t, a)
        check(f"MVVM off: {t} -> MVVM_NOT_ENABLED, nothing created", not r["ok"] and r.get("errorCode") == "MVVM_NOT_ENABLED"
              and not c("asset_exists", {"path": W})["data"]["exists"], r)
    r = c("viewmodel_list", {})
    check("MVVM off: missing argument is still checked first", not r["ok"] and r.get("errorCode") == "MISSING_ARGUMENT", r)
    # the old tools keep working without MVVM
    delete_if_exists(W)
    r = c("widget_create", {"path": W})
    check("MVVM off: widget_create still works", r["ok"], r)
    r = c("widget_add", {"path": W, "class": "TextBlock", "name": "Gold"})
    check("MVVM off: widget_add still works", r["ok"], r)
    r = c("widget_tree", {"path": W})
    check("MVVM off: widget_tree shows the widget", r["ok"] and any(x["name"] == "Gold" for x in r["data"]["widgets"]), r)
    delete_if_exists(W)
    done()

# -------------------------------------------------------------------- MVVM on
r = c("object_get", {"path": VM, "classDefault": True, "properties": ["Gold"]})
if not r["ok"]:
    print("SKIP the MVVM on-state checks: copy tests/fixtures/EWTestViewModel.h/.cpp into the test project module (needs ModelViewViewModel + FieldNotification in its Build.cs)")
    done()

delete_if_exists(W)
r = c("widget_create", {"path": W})
check("widget_create /Game/_EWTest/WBP_EWShop", r["ok"], r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "Gold", "properties": {"Text": "0"}})
check("widget_add TextBlock Gold", r["ok"], r)
r = c("widget_add", {"path": W, "class": "TextBlock", "name": "Label"})
check("widget_add TextBlock Label", r["ok"], r)


def listing():
    r = c("viewmodel_list", {"widget": W})
    assert r["ok"], r
    return r["data"]


d = listing()
check("viewmodel_list on a fresh widget: no view models, no bindings", d["viewModelCount"] == 0 and d["bindingCount"] == 0 and d["viewModels"] == [] and d["bindings"] == [], d)
r = c("viewmodel_list", {"widget": ROOT + "/WBP_Nope"})
check("viewmodel_list unknown widget -> ASSET_NOT_FOUND", not r["ok"] and r["errorCode"] == "ASSET_NOT_FOUND", r)

# --- viewmodel_add
r = c("viewmodel_add", {"widget": W, "viewModel": "/Script/EnginewrightTest.NoSuchViewModel"})
check("add unknown class -> CLASS_NOT_FOUND", not r["ok"] and r["errorCode"] == "CLASS_NOT_FOUND", r)
r = c("viewmodel_add", {"widget": W, "viewModel": "/Script/Engine.Actor"})
check("add a class that is not a view model -> INVALID_VIEWMODEL", not r["ok"] and r["errorCode"] == "INVALID_VIEWMODEL", r)
r = c("viewmodel_add", {"widget": W, "viewModel": VM, "creationType": "Sometimes"})
check("add with a bad creationType -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("viewmodel_add", {"widget": W, "viewModel": VM, "creationType": "GlobalViewModelCollection"})
check("GlobalViewModelCollection without globalIdentifier -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
check("failed adds changed nothing", listing()["viewModelCount"] == 0)

r = c("viewmodel_add", {"widget": W, "viewModel": VM})
vm = (r.get("data") or {}).get("viewModel", {})
check("viewmodel_add: added, named after the class, CreateInstance, compiled, saved, read back",
      r["ok"] and r["data"]["added"] and vm.get("name") == "EWTestViewModel" and vm.get("creationType") == "CreateInstance"
      and vm.get("class", "").endswith("EWTestViewModel") and r["data"]["compiled"] and r["data"]["saved"]
      and r["data"]["readBack"]["viewModelCount"] == 1, r)
r = c("viewmodel_add", {"widget": W, "viewModel": "EWTestViewModel"})
check("same class + same name again (short class name) -> ok, added false", r["ok"] and r["data"]["added"] is False and listing()["viewModelCount"] == 1, r)
r = c("viewmodel_add", {"widget": W, "viewModel": VM, "name": "Shop", "creationType": "Manual"})
check("second instance with its own name and creationType Manual", r["ok"] and r["data"]["added"] and r["data"]["viewModel"]["name"] == "Shop"
      and r["data"]["viewModel"]["creationType"] == "Manual" and listing()["viewModelCount"] == 2, r)
r = c("viewmodel_add", {"widget": W, "viewModel": VM, "name": "Gold"})
check("a name that a widget already uses -> INVALID_NAME, nothing added", not r["ok"] and r["errorCode"] == "INVALID_NAME" and listing()["viewModelCount"] == 2, r)
r = c("viewmodel_remove", {"widget": W, "viewModel": "Shop"})
check("viewmodel_remove Shop", r["ok"] and r["data"]["removed"]["name"] == "Shop" and listing()["viewModelCount"] == 1, r)
r = c("viewmodel_remove", {"widget": W, "viewModel": "Shop"})
check("remove again -> VIEWMODEL_NOT_FOUND lists the view models", not r["ok"] and r["errorCode"] == "VIEWMODEL_NOT_FOUND" and "EWTestViewModel" in r.get("suggestions", []), r)

# --- viewmodel_bind: refusals first (nothing may be written)
def bad_bind(label, code, **kw):
    a = {"widget": W, "viewModel": "EWTestViewModel", "source": "Gold", "target": "Gold.Text"}
    a.update(kw)
    r = c("viewmodel_bind", a)
    check(f"bind {label} -> {code}, no binding written", not r["ok"] and r.get("errorCode") == code and listing()["bindingCount"] == 0, r)
    return r


r = bad_bind("int into a Text without conversion", "TYPE_MISMATCH")
check("TYPE_MISMATCH names both types", "int32" in r["message"] and "Text" in r["message"], r)
bad_bind("a source that is not FieldNotify", "SOURCE_NOT_NOTIFY", source="Silver", conversion="Conv_IntToText")
bad_bind("unknown widget", "WIDGET_NOT_FOUND", target="Nope.Text")
bad_bind("unknown widget property", "PROPERTY_NOT_FOUND", target="Gold.Nope")
bad_bind("unknown view model property", "PROPERTY_NOT_FOUND", source="Gld")
bad_bind("unknown view model", "VIEWMODEL_NOT_FOUND", viewModel="Nope")
bad_bind("a target without a property", "INVALID_ARGUMENT", target="Gold")
bad_bind("a mode that does not exist", "INVALID_ARGUMENT", mode="Sideways")
bad_bind("a conversion that does not exist", "CONVERSION_NOT_FOUND", conversion="Conv_NoSuchThing")
bad_bind("a conversion of other types", "TYPE_MISMATCH", conversion="Conv_BoolToText")

# --- viewmodel_bind: the acceptance case
r = c("viewmodel_bind", {"widget": W, "viewModel": "EWTestViewModel", "source": "Gold", "target": "Gold.Text", "conversion": "Conv_IntToText"})
b = (r.get("data") or {}).get("binding", {})
back = ((r.get("data") or {}).get("readBack") or {}).get("bindings", [])
check("bind Gold -> Gold.Text with Conv_IntToText: added, mode default, compiled, saved, no errors",
      r["ok"] and r["data"]["added"] and b.get("source") == "EWTestViewModel.Gold" and b.get("target") == "Gold.Text"
      and b.get("mode") == "OneWayToDestination" and b.get("conversion") == "Conv_IntToText" and r["data"]["compiled"] and r["data"]["saved"]
      and len(back) == 1 and back[0]["errors"] == [], r)
r = c("viewmodel_bind", {"widget": W, "viewModel": "EWTestViewModel", "source": "Gold", "target": "Gold.Text", "conversion": "Conv_IntToText"})
check("same binding again -> ok, added false, still one binding", r["ok"] and r["data"]["added"] is False and listing()["bindingCount"] == 1, r)
r = c("viewmodel_bind", {"widget": W, "viewModel": "EWTestViewModel", "source": "Silver", "target": "Gold.Text", "mode": "OneTimeToDestination", "conversion": "Conv_IntToText"})
check("another source for the same target -> BINDING_EXISTS", not r["ok"] and r["errorCode"] == "BINDING_EXISTS" and listing()["bindingCount"] == 1, r)
r = c("viewmodel_bind", {"widget": W, "viewModel": "EWTestViewModel", "source": "Title", "target": "Label.Text"})
check("Text -> Text needs no conversion", r["ok"] and r["data"]["added"] and r["data"]["binding"]["conversion"] == "", r)
r = c("viewmodel_bind", {"widget": W, "viewModel": "EWTestViewModel", "source": "Silver", "target": "Label.Text", "mode": "OneTimeToDestination", "conversion": "Conv_IntToText"})
check("an existing target with another source -> BINDING_EXISTS", not r["ok"] and r["errorCode"] == "BINDING_EXISTS", r)
r = c("viewmodel_bind", {"widget": W, "viewModel": "EWTestViewModel", "source": "Title", "target": "Label.Text", "mode": "OneTimeToDestination"})
check("same source and target with another mode updates the binding", r["ok"] and r["data"]["added"] is False and r["data"]["binding"]["mode"] == "OneTimeToDestination"
      and listing()["bindingCount"] == 2, r)
d = listing()
check("viewmodel_list shows both bindings with source -> target", sorted((x["source"], x["target"]) for x in d["bindings"]) == [("EWTestViewModel.Gold", "Gold.Text"), ("EWTestViewModel.Title", "Label.Text")], d)

# --- remove / unbind
r = c("viewmodel_remove", {"widget": W, "viewModel": "EWTestViewModel"})
check("remove a view model that bindings use -> VIEWMODEL_IN_USE lists them", not r["ok"] and r["errorCode"] == "VIEWMODEL_IN_USE" and len(r.get("suggestions", [])) == 2
      and listing()["viewModelCount"] == 1, r)
r = c("viewmodel_unbind", {"widget": W, "target": "Nope.Text"})
check("unbind a target with no binding -> BINDING_NOT_FOUND lists the targets", not r["ok"] and r["errorCode"] == "BINDING_NOT_FOUND" and "Gold.Text" in r.get("suggestions", []), r)
r = c("viewmodel_unbind", {"widget": W, "target": "Gold.Text", "source": "Title"})
check("unbind with a source filter that does not match -> BINDING_NOT_FOUND", not r["ok"] and r["errorCode"] == "BINDING_NOT_FOUND" and listing()["bindingCount"] == 2, r)

# one undo step: a failing step after a bind rolls the bind back (atomic batch = one transaction)
r = c("batch", {"steps": [
    {"tool": "viewmodel_unbind", "args": {"widget": W, "target": "Label.Text"}},
    {"tool": "viewmodel_unbind", "args": {"widget": W, "target": "Nope.Text"}}]})
check("atomic batch: unbind then a failing step -> the unbind is undone", not r["ok"] and r["data"].get("rolledBack") and listing()["bindingCount"] == 2, r)

r = c("viewmodel_unbind", {"widget": W, "target": "Label.Text"})
check("viewmodel_unbind Label.Text", r["ok"] and len(r["data"]["removed"]) == 1 and r["data"]["compiled"] and listing()["bindingCount"] == 1, r)
r = c("viewmodel_remove", {"widget": W, "viewModel": "EWTestViewModel", "removeBindings": True})
d = listing()
check("viewmodel_remove with removeBindings: view model and its binding are gone", r["ok"] and r["data"]["bindingsRemoved"] == 1 and d["viewModelCount"] == 0 and d["bindingCount"] == 0, [r, d])
r = c("widget_tree", {"path": W})
check("the widget tree is untouched by all of this", r["ok"] and {x["name"] for x in r["data"]["widgets"]} >= {"Gold", "Label"}, r)

delete_if_exists(W)
check("fixture deleted", not c("asset_exists", {"path": W})["data"]["exists"])
done()
