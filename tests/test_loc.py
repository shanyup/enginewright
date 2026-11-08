"""Localization tests: String Table assets (create, set, remove, read back), registry listing, source vs translation
comparison, and switching the editor language. Writes only under /Game/_EWTest.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, delete_if_exists, done, ensure_stopped

ensure_stopped()
SRC, TR = ROOT + "/ST_EWSource", ROOT + "/ST_EWSource_TR"
delete_if_exists(SRC, TR)

r = c("stringtable_set", {"path": SRC, "entries": {"A": "x"}})
check("set on a missing table without create -> TABLE_NOT_FOUND", not r["ok"] and r["errorCode"] == "TABLE_NOT_FOUND", r)
r = c("stringtable_set", {"path": SRC, "create": True, "namespace": "EWTest", "entries": {
    "Menu.Play": "Play", "Menu.Quit": "Quit", "Gold": "You have {0} gold", "Hint": "Press <key>E</> to cook", "Name.Loki": "Loki", "Only.Source": "Only here"}})
check("create a String Table with entries, read back", r["ok"] and r["data"]["created"] and r["data"]["entries"] == 6 and r["data"]["namespace"] == "EWTest", r)
r = c("stringtable_set", {"path": TR, "create": True, "entries": {
    "Menu.Play": "Oyna", "Menu.Quit": "", "Gold": "{1} altının var", "Hint": "Pişirmek için <key>E</> bas", "Name.Loki": "Loki", "Extra.Key": "fazla"}})
check("create the translation table", r["ok"] and r["data"]["entries"] == 6, r)
r = c("stringtable_set", {"path": SRC, "entries": {"Menu.Play": "Play!"}, "remove": ["Only.Source"]})
check("set + remove on an existing table, read back", r["ok"] and not r["data"]["created"] and r["data"]["entries"] == 5 and r["data"]["removed"] == 1, r)
r = c("stringtable_set", {"path": SRC, "remove": ["Nope.Key"]})
check("remove an unknown key -> KEY_NOT_FOUND, nothing changed", not r["ok"] and r["errorCode"] == "KEY_NOT_FOUND"
      and c("stringtable_get", {"table": SRC})["data"]["tableEntries"] == 5, r)
r = c("stringtable_set", {"path": SRC, "entries": {"": "empty key"}})
check("empty key -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("stringtable_set", {"path": SRC, "entries": {"N": 5}})
check("non-string value -> error", not r["ok"], r)

r = c("stringtable_get", {"table": SRC, "keys": ["Menu.Play", "Missing.One"]})
check("get by keys reports the source and the missing key", r["ok"] and r["data"]["entries"][0]["source"] == "Play!" and r["data"]["missingKeys"] == ["Missing.One"], r)
r = c("stringtable_get", {"table": SRC, "match": "Menu.*"})
check("get by key wildcard", r["ok"] and r["data"]["matched"] == 2, r)
r = c("stringtable_get", {"table": SRC, "keys": ["Menu.Ply"]})
check("get only unknown keys -> KEY_NOT_FOUND with suggestion", not r["ok"] and r["errorCode"] == "KEY_NOT_FOUND" and "Menu.Play" in r.get("suggestions", []), r)
r = c("stringtable_get", {"table": "ST_DoesNotExist"})
check("unknown table id -> TABLE_NOT_FOUND", not r["ok"] and r["errorCode"] == "TABLE_NOT_FOUND", r)
r = c("stringtable_get", {"table": ROOT})
check("a folder is not a table -> TABLE_NOT_FOUND", not r["ok"] and r["errorCode"] == "TABLE_NOT_FOUND", r)

r = c("stringtable_list", {"filter": "*ST_EWSource*"})
ids = [t["id"] for t in r["data"]["tables"]] if r["ok"] else []
check("list shows both asset tables with their asset path", r["ok"] and len(ids) == 2 and all(t.get("asset", "").startswith(ROOT) for t in r["data"]["tables"]), r)

r = c("stringtable_compare", {"source": SRC, "translation": TR, "ignoreSame": ["Name.*"]})
d = r.get("data") or {}
cn = d.get("counts", {})
check("compare: missing / extra / empty counted", r["ok"] and cn.get("missing") == 0 and cn.get("extra") == 1 and cn.get("empty") == 1, d)
check("compare: format argument mismatch {0} vs {1}", any(x["key"] == "Gold" for x in d.get("formatMismatch", [])) and not any(x["key"] == "Hint" for x in d.get("formatMismatch", [])), d.get("formatMismatch"))
check("compare: ignoreSame keeps proper names out of sameAsSource", cn.get("sameAsSource") == 0 and d.get("clean") is False, d)
r = c("stringtable_compare", {"source": SRC, "translation": TR})
check("compare without ignoreSame flags the identical entry", r["ok"] and any(x["key"] == "Name.Loki" for x in r["data"]["sameAsSource"]), r)
r = c("stringtable_compare", {"source": SRC, "translation": SRC})
check("compare a table with itself -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)

r = c("loc_culture")
check("loc_culture reads the language", r["ok"] and r["data"]["language"], r)
before = r["data"]["language"] if r["ok"] else "en"
r = c("loc_culture", {"language": "tr"})
check("loc_culture switches to Turkish (read back)", r["ok"] and r["data"]["language"].startswith("tr") and r["data"]["before"] == before, r)
r = c("loc_culture", {"language": before})
check("loc_culture switches back", r["ok"] and r["data"]["language"] == before, r)
r = c("loc_culture", {"language": "zz-Nowhere"})
check("loc_culture unknown culture -> error, language unchanged", not r["ok"] and c("loc_culture")["data"]["language"] == before, r)

if not os.environ.get("EW_KEEP_FIXTURES"):
    delete_if_exists(SRC, TR)
done()
