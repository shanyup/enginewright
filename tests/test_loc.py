"""Localization tests: String Table assets (create, set, remove, read back), registry listing, source vs translation
comparison, and switching the editor language. Writes only under /Game/_EWTest.
"""
import os, shutil, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import PROJ, ROOT, call as c, check, delete_if_exists, done, ensure_stopped

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

# ---------------------------------------------------------------- ER-10: .po / .locres / CSV inputs
LOC = PROJ / "Saved" / "Enginewright" / "loc_test"   # this test's own folder, removed at the end
shutil.rmtree(LOC, ignore_errors=True)
LOC.mkdir(parents=True)
PO = LOC / "Game.po"
PO.write_text('''msgid ""
msgstr ""
"Language: tr\\n"

#. Key:\tMenu.Play
msgctxt "EWTest,Menu.Play"
msgid "Play"
msgstr "Oyna"

#. Key:\tMenu.Quit
msgctxt "EWTest,Menu.Quit"
msgid "Quit"
msgstr ""

#. Key:\tGold
msgctxt "EWTest,Gold"
msgid "You have {0} gold"
msgstr "{1} altının var"

#, fuzzy
#. Key:\tHint
msgctxt "EWTest,Hint"
msgid "Press <key>E</> to cook"
msgstr "Pişirmek için <key>E</> bas"

#. Key:\tName.Loki
msgctxt "EWTest,Name.Loki"
msgid "Loki"
msgstr "Loki"

#. Key:\tMulti
msgctxt "EWTest,Multi"
msgid "He said \\"hi\\", {0}"
msgstr ""
"O dedi ki \\"merhaba\\", "
"{1}"

#. Key:\tExtra.Key
msgctxt "EWTest,Extra.Key"
msgid "extra"
msgstr "fazla"

#. Key:\tMenu.Play
msgctxt "Other,Menu.Play"
msgid "Play!"
msgstr "WRONG"

#~ msgctxt "EWTest,Old.Gone"
#~ msgid "gone"
#~ msgstr "gitti"
''', encoding="utf-8")
CSV = LOC / "Source.csv"
CSV.write_text('Key,SourceString,Comment\nMenu.Play,Play!,\nMenu.Quit,Quit,\nGold,You have {0} gold,\nHint,Press <key>E</> to cook,\n'
               'Name.Loki,Loki,\nNew.Key,Brand new,\nMulti,"He said ""hi"", {0}",\n', encoding="utf-8")


def crc32_ue(s):
    """FCrc::StrCrc32 over the characters (the hash a .locres stores for the source text): CRC-32 of the text as 32-bit characters."""
    return zlib.crc32(s.encode("utf-32-le"))


def fstr(s):
    return struct.pack("<i", -(len(s) + 1)) + (s + "\0").encode("utf-16-le")


def legacy_locres(path, ns, entries):
    """entries: key -> (source text the hash is made from, localized text). Legacy .locres layout."""
    body = struct.pack("<I", 1) + fstr(ns) + struct.pack("<I", len(entries))
    for k, (src, loc) in entries.items():
        body += fstr(k) + struct.pack("<I", crc32_ue(src)) + fstr(loc)
    path.write_bytes(body)


rel_po = "Saved/Enginewright/loc_test/Game.po"
r = c("stringtable_compare", {"source": SRC, "po": rel_po})
d = r.get("data") or {}
cn = d.get("counts", {})
check("po (project-relative path): kind, entry count without the other namespace", r["ok"] and d.get("translationKind") == "po" and d.get("translationEntries") == 7, r)
check("po: empty translation counted, extra keys counted (Multi, Extra.Key), nothing missing, obsolete entry ignored",
      cn.get("empty") == 1 and cn.get("extra") == 2 and cn.get("missing") == 0 and not any(x["key"] == "Old.Gone" for x in d.get("extra", [])), d)
check("po: format argument {0} vs {1} found", [x["key"] for x in d.get("formatMismatch", [])] == ["Gold"], d.get("formatMismatch"))
check("po: identical-to-source entry listed, with and without ignoreSame",
      [x["key"] for x in d.get("sameAsSource", [])] == ["Name.Loki"]
      and c("stringtable_compare", {"source": SRC, "po": str(PO), "ignoreSame": ["Name.*"]})["data"]["counts"]["sameAsSource"] == 0, d.get("sameAsSource"))
check("po: translation made for an older source text -> sourceChanged (Play vs Play!)",
      [x["key"] for x in d.get("sourceChanged", [])] == ["Menu.Play"] and d["sourceChanged"][0].get("translatedFrom") == "Play", d.get("sourceChanged"))
check("po: fuzzy entry reported; the Other namespace was not used (Menu.Play is Oyna, not WRONG)",
      [x["key"] for x in d.get("fuzzy", [])] == ["Hint"] and d["sourceChanged"][0].get("translation") == "Oyna" and d.get("clean") is False, d)

r = c("stringtable_compare", {"sourceCsv": str(CSV), "po": str(PO), "namespace": "EWTest"})
d = r.get("data") or {}
cn = d.get("counts", {})
check("CSV source + po: missing key found, counts", r["ok"] and [x["key"] for x in d.get("missing", [])] == ["New.Key"] and d.get("sourceEntries") == 7 and cn.get("extra") == 1, r)
mf = {x["key"]: x for x in d.get("formatMismatch", [])}
check("po: escaped quotes and a msgstr on continuation lines are read whole; CSV quotes too",
      mf.get("Multi", {}).get("translation") == 'O dedi ki "merhaba", {1}' and mf.get("Multi", {}).get("source") == 'He said "hi", {0}', mf)
check("CSV source: rich text tags match (Hint has no format difference)", "Hint" not in mf, mf)
r = c("stringtable_compare", {"sourceCsv": str(CSV), "po": str(PO)})
check("source without a namespace (CSV): keys are matched in any namespace, no warning", r["ok"] and not r.get("warnings") and r["data"]["translationEntries"] == 7, r)
r = c("stringtable_compare", {"sourceCsv": str(CSV), "po": str(PO), "namespace": "Nope"})
check("namespace not found in the po -> compared across namespaces, with a warning", r["ok"] and r.get("warnings") and r["data"]["translationEntries"] == 7, r)
r = c("stringtable_compare", {"sourceCsv": str(CSV), "po": str(PO), "namespace": "*"})
check("namespace * = any namespace (first entry of a key wins)", r["ok"] and r["data"]["translationEntries"] == 7 and not r.get("warnings"), r)

LR = LOC / "Game.locres"
legacy_locres(LR, "EWTest", {"Menu.Play": ("Play!", "Oyna"), "Menu.Quit": ("Quit", "Çıkış"), "Gold": ("Gold", "{0} altın")})
r = c("stringtable_compare", {"source": SRC, "locres": str(LR)})
d = r.get("data") or {}
cn = d.get("counts", {})
check("locres: read, kind and entry count", r["ok"] and d.get("translationKind") == "locres" and d.get("translationEntries") == 3, r)
check("locres: keys the file lacks are missing (Hint, Name.Loki)", sorted(x["key"] for x in d.get("missing", [])) == ["Hint", "Name.Loki"], d.get("missing"))
check("locres: source hash differs -> sourceChanged (Gold only; the others hash to what the file stores)", [x["key"] for x in d.get("sourceChanged", [])] == ["Gold"], d.get("sourceChanged"))

r = c("stringtable_compare", {"source": SRC})
check("no translation input -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("stringtable_compare", {"source": SRC, "po": str(PO), "translation": TR})
check("two translation inputs -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("stringtable_compare", {"po": str(PO)})
check("no source -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("stringtable_compare", {"source": SRC, "sourceCsv": str(CSV), "po": str(PO)})
check("source and sourceCsv together -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("stringtable_compare", {"source": SRC, "po": str(LOC / "Nope.po")})
check("po file that does not exist -> FILE_NOT_FOUND", not r["ok"] and r["errorCode"] == "FILE_NOT_FOUND", r)
r = c("stringtable_compare", {"source": SRC, "po": str(CSV)})
check("po that is not a .po file -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
BAD = LOC / "Bad.locres"
BAD.write_bytes(b"this is not a locres file at all")
r = c("stringtable_compare", {"source": SRC, "locres": str(BAD)})
check("garbage .locres -> INVALID_FILE", not r["ok"] and r["errorCode"] == "INVALID_FILE", r)
EMPTYPO = LOC / "Empty.po"
EMPTYPO.write_text('msgid ""\nmsgstr ""\n', encoding="utf-8")
r = c("stringtable_compare", {"source": SRC, "po": str(EMPTYPO)})
check("a po without translated entries -> INVALID_FILE", not r["ok"] and r["errorCode"] == "INVALID_FILE", r)
BADCSV = LOC / "Bad.csv"
BADCSV.write_text("foo\nbar\n", encoding="utf-8")
r = c("stringtable_compare", {"sourceCsv": str(BADCSV), "po": str(PO)})
check("CSV without Key/SourceString columns -> INVALID_FILE", not r["ok"] and r["errorCode"] == "INVALID_FILE", r)
r = c("stringtable_compare", {"source": SRC, "translation": SRC})
check("table vs itself is still refused", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
shutil.rmtree(LOC, ignore_errors=True)
check("test files removed", not LOC.exists())

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
