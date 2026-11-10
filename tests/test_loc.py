"""Localization tests: String Table assets (create, set, remove, read back), registry listing, source vs translation
comparison, and switching the editor language. Writes only under /Game/_EWTest.
"""
import os, pathlib, shutil, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import PROJ, ROOT, call as c, check, delete_if_exists, done, ensure_stopped, rpc

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
check("source without a namespace (CSV): the CSV file name is the default namespace, the po has no 'Source' namespace -> matched in any namespace, with a note",
      r["ok"] and r["data"]["translationEntries"] == 7 and r["data"].get("namespace") == "Source"
      and any("keys were matched in any namespace" in w for w in r.get("warnings", [])), r)
r = c("stringtable_compare", {"sourceCsv": str(CSV), "po": str(PO), "namespace": "Nope"})
check("namespace not found in the po -> compared across namespaces, with a warning", r["ok"] and r.get("warnings") and r["data"]["translationEntries"] == 7, r)
r = c("stringtable_compare", {"sourceCsv": str(CSV), "po": str(PO), "namespace": "*"})
check("namespace * = any namespace (first entry of a key wins)", r["ok"] and r["data"]["translationEntries"] == 7 and not r.get("warnings"), r)

# slice H3: CSV source picks the CSV file name as the default namespace; a wrong namespace warns where the keys are
CSVNS = LOC / "ST_EWNs.csv"
CSVNS.write_text("Key,SourceString\nA,Apple\nB,Bread\n", encoding="utf-8")
PONS = LOC / "NsGuid.po"
PONS.write_text('''msgid ""
msgstr ""
"Language: tr\\n"

#. Key:\\tGUID1
msgctxt ",GUID1"
msgid "Other"
msgstr "Diğe\\u0301r"

#. Key:\\tA
msgctxt "ST_EWNs,A"
msgid "Apple"
msgstr "Elma"

#. Key:\\tB
msgctxt "ST_EWNs,B"
msgid "Bread"
msgstr "Ekmek"
''', encoding="utf-8")
r = c("stringtable_compare", {"sourceCsv": str(CSVNS), "po": str(PONS)})
d = r.get("data") or {}
cn = d.get("counts", {})
check("CSV source without namespace: the CSV file name (ST_EWNs) is the namespace used, every key found",
      r["ok"] and d.get("namespace") == "ST_EWNs" and cn.get("missing") == 0 and d.get("translationEntries") == 2
      and cn.get("extra") == 1, r)
check("the po's empty-namespace GUID entry did not hijack the namespace (no warnings)", r["ok"] and not r.get("warnings"), r)
PONS2 = LOC / "NsOther.po"
PONS2.write_text('''msgid ""
msgstr ""
"Language: tr\\n"

#. Key:\\tGUID1
msgctxt ",GUID1"
msgid "Other"
msgstr "Diğe\\u0301r"

#. Key:\\tA
msgctxt "Other,A"
msgid "Apple"
msgstr "Elma"

#. Key:\\tB
msgctxt "Other,B"
msgid "Bread"
msgstr "Ekmek"
''', encoding="utf-8")
r = c("stringtable_compare", {"sourceCsv": str(CSVNS), "po": str(PONS2)})
d = r.get("data") or {}
check("CSV name namespace has no entries -> fell back to any-namespace matching, nothing missing, note explains",
      r["ok"] and d.get("namespace") == "ST_EWNs" and d["counts"].get("missing") == 0
      and any("No entry in namespace 'ST_EWNs'" in w and "any namespace" in w for w in r.get("warnings", [])), r)


def multi_ns_locres(path, by_ns):
    """Multiple namespaces in one legacy .locres, same layout legacy_locres writes."""
    body = struct.pack("<I", len(by_ns))
    for ns, ents in by_ns.items():
        body += fstr(ns) + struct.pack("<I", len(ents))
        for k, (src, loc) in ents.items():
            body += fstr(k) + struct.pack("<I", crc32_ue(src)) + fstr(loc)
    path.write_bytes(body)


WEIRD = LOC / "MostMissing.locres"
multi_ns_locres(WEIRD, {"ST_EWNs2": {"A": ("Apple", "Elma")}, "Other": {"B": ("Bread", "Ekmek"), "C": ("Cake", "Kek")}})
CSVW = LOC / "ST_EWNs2.csv"
CSVW.write_text("Key,SourceString\nA,Apple\nB,Bread\nC,Cake\n", encoding="utf-8")
r = c("stringtable_compare", {"sourceCsv": str(CSVW), "locres": str(WEIRD)})
d = r.get("data") or {}
check("locres: more than half the source keys missing in the default namespace -> a warning lists where the keys are",
      r["ok"] and d.get("namespace") == "ST_EWNs2" and d["counts"].get("missing") == 2 and d["counts"].get("extra") == 0
      and any(w.startswith("Most source keys are missing in namespace 'ST_EWNs2'") and "The file has them in: Other (2 keys)" in w
              and w.rstrip().endswith("Pass namespace.") for w in r.get("warnings", [])), r.get("warnings"))
r = c("stringtable_compare", {"sourceCsv": str(CSVW), "locres": str(WEIRD), "namespace": "*"})
d = r.get("data") or {}
check("the most-missing warning is skipped for namespace *", r["ok"] and d["counts"].get("missing") == 0
      and all("Most source keys" not in w for w in r.get("warnings", [])), r["warnings"])

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

# ---------------------------------------------------------------- ER-13: font_coverage
FONTS = PROJ / "Saved" / "Enginewright" / "font_test"   # this test's own folder, removed at the end
shutil.rmtree(FONTS, ignore_errors=True)
FONTS.mkdir(parents=True)


def u16(v): return struct.pack(">H", v)
def u32(v): return struct.pack(">I", v)


def sfnt(cmap):
    """A TrueType file that holds only a cmap table (all font_coverage reads)."""
    head = u32(0x00010000) + u16(1) + u16(16) + u16(0) + u16(0)
    rec = b"cmap" + u32(0) + u32(12 + 16) + u32(len(cmap))
    return head + rec + cmap


def cmap_format4(segments, arrays=None):
    """segments: [(start, end, kind, value)]: kind 'delta' (glyph = cp + value) or 'array' (value = index into arrays)."""
    segs = segments + [(0xFFFF, 0xFFFF, "delta", 1)]
    n = len(segs)
    ends = b"".join(u16(s[1]) for s in segs)
    starts = b"".join(u16(s[0]) for s in segs)
    deltas = b"".join(u16(s[3] & 0xFFFF) if s[2] == "delta" else u16(0) for s in segs)
    offs = b""
    for i, s in enumerate(segs):
        offs += u16(0) if s[2] == "delta" else u16((n - i) * 2 + s[3] * 2)
    glyphs = b"".join(u16(g) for g in (arrays or []))
    body = u16(n * 2) + u16(0) + u16(0) + u16(0) + ends + u16(0) + starts + deltas + offs + glyphs
    sub = u16(4) + u16(6 + len(body)) + u16(0) + body
    return u16(0) + u16(1) + u16(3) + u16(1) + u32(12) + sub


def cmap_format12(groups):
    body = b"".join(u32(s) + u32(e) + u32(g) for s, e, g in groups)
    sub = u16(12) + u16(0) + u32(16 + len(body)) + u32(0) + u32(len(groups)) + body
    return u16(0) + u16(1) + u16(3) + u16(10) + u32(12) + sub


TR = "\u00c7\u011e\u0130\u00d6\u015e\u00dc\u00e7\u011f\u0131\u00f6\u015f\u00fc"   # Ç Ğ İ Ö Ş Ü ç ğ ı ö ş ü
# Latin + Ç ç ö ü ğ ı ş as single segments, Ö and Ü through a glyph array (gaps = glyph 0); Ğ İ Ş are missing.
f4 = [(0x41, 0x5A, "delta", 1 - 0x41), (0x61, 0x7A, "delta", 27 - 0x61), (0xC7, 0xC7, "delta", 100 - 0xC7), (0xD6, 0xDC, "array", 0),
      (0xE7, 0xE7, "delta", 101 - 0xE7), (0xF6, 0xF6, "delta", 102 - 0xF6), (0xFC, 0xFC, "delta", 103 - 0xFC),
      (0x11F, 0x11F, "delta", 104 - 0x11F), (0x131, 0x131, "delta", 105 - 0x131), (0x15F, 0x15F, "delta", 106 - 0x15F)]
(FONTS / "Partial.ttf").write_bytes(sfnt(cmap_format4(f4, [110, 0, 0, 0, 0, 0, 111])))
(FONTS / "Wide.otf").write_bytes(sfnt(cmap_format12([(0x41, 0x5A, 1), (0x61, 0x7A, 27), (0xC7, 0x15F, 60), (0x1F600, 0x1F64F, 400)])))
(FONTS / "NotAFont.ttf").write_bytes(b"definitely not a font file")

r = c("font_coverage", {"font": str(FONTS / "Partial.ttf"), "culture": "tr"})
d = r.get("data") or {}
check("font_coverage: file font, culture tr -> exactly Ğ İ Ş missing (format 4 segments, glyph array, gaps)",
      r["ok"] and [m["codePoint"] for m in d.get("missing", [])] == ["U+011E", "U+0130", "U+015E"] and d.get("missingText") == "\u011e\u0130\u015e"
      and d.get("kind") == "file" and d.get("clean") is False and d.get("language") == "tr", r)
check("font_coverage: counts (tr = 52 basic Latin + 12 Turkish letters, 3 missing) and the face list",
      d.get("checked") == 64 and d.get("covered") == 61 and d.get("missingCount") == 3 and len(d.get("faces", [])) == 1 and d["faces"][0]["missingCount"] == 3, d)
r = c("font_coverage", {"font": "Saved/Enginewright/font_test/Partial.ttf", "chars": TR})
d = r.get("data") or {}
check("font_coverage: project-relative path, chars = the Turkish letters -> the same three missing, 9 of 12 covered",
      r["ok"] and d.get("covered") == 9 and d.get("checked") == 12 and d.get("missingText") == "\u011e\u0130\u015e", r)
r = c("font_coverage", {"font": str(FONTS / "Wide.otf"), "culture": "tr"})
check("font_coverage: format 12 font covers every Turkish letter -> clean", r["ok"] and r["data"]["clean"] is True and r["data"]["missingCount"] == 0, r)
r = c("font_coverage", {"font": str(FONTS / "Wide.otf"), "chars": "A \u00c7 \U0001F600 \U0001F680 \u3042"})
d = r.get("data") or {}
check("font_coverage: supplementary-plane characters (surrogate pairs), spaces skipped; emoji drawn, rocket and kana not",
      r["ok"] and d.get("checked") == 5 and [m["codePoint"] for m in d["missing"]] == ["U+1F680", "U+3042"], r)
r = c("font_coverage", {"font": str(FONTS / "Wide.otf"), "chars": "\U0001F600\U0001F600A"})
check("font_coverage: repeated characters are counted once", r["ok"] and r["data"]["checked"] == 2, r)

r = c("font_coverage", {"font": "/Engine/EngineFonts/Roboto", "culture": "tr"})
d = r.get("data") or {}
check("font_coverage: a Font asset (the engine's Roboto) is read through its faces; Turkish is covered",
      r["ok"] and d.get("kind") == "font" and len(d.get("faces", [])) >= 1 and d.get("clean") is True, r)
r = c("font_coverage", {"font": "/Engine/EngineFonts/Roboto", "chars": "\u3042\ue123"})
d = r.get("data") or {}
dflt = [f for f in d.get("faces", []) if f.get("typeface") == "default"]
check("font_coverage: a character only the fallback typeface draws (kana) is covered; the default faces list it as missing; a private-use character is missing overall",
      r["ok"] and dflt and all(f.get("missingCount") == 2 for f in dflt) and [m["codePoint"] for m in d["missing"]] == ["U+E123"]
      and any(f.get("typeface") == "fallback" and f.get("missingCount") == 1 for f in d["faces"]), r)

r = c("font_coverage", {"font": str(FONTS / "Partial.ttf")})
check("neither culture nor chars -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("font_coverage", {"font": str(FONTS / "Partial.ttf"), "culture": "tr", "chars": "x"})
check("both culture and chars -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("font_coverage", {"font": str(FONTS / "Partial.ttf"), "culture": "zz-Nowhere"})
check("unknown culture -> UNKNOWN_CULTURE", not r["ok"] and r["errorCode"] == "UNKNOWN_CULTURE", r)
r = c("font_coverage", {"font": str(FONTS / "Partial.ttf"), "culture": "ja"})
check("a culture without a letter list -> NO_CHAR_TABLE with the known languages", not r["ok"] and r["errorCode"] == "NO_CHAR_TABLE" and "tr" in r.get("suggestions", []), r)
r = c("font_coverage", {"font": str(FONTS / "Nope.ttf"), "culture": "tr"})
check("font file that does not exist -> FILE_NOT_FOUND", not r["ok"] and r["errorCode"] == "FILE_NOT_FOUND", r)
r = c("font_coverage", {"font": str(FONTS / "NotAFont.ttf"), "culture": "tr"})
check("a file that is not a font -> UNSUPPORTED_FONT", not r["ok"] and r["errorCode"] == "UNSUPPORTED_FONT", r)
r = c("font_coverage", {"font": str(FONTS / "Partial.txt"), "culture": "tr"})
check("font with another extension -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("font_coverage", {"font": "/Game/_EWTest/Nope_Font", "culture": "tr"})
check("unknown font asset -> ASSET_NOT_FOUND", not r["ok"] and r["errorCode"] == "ASSET_NOT_FOUND", r)
r = c("font_coverage", {"font": SRC, "culture": "tr"})
check("an asset that is not a font -> NOT_A_FONT", not r["ok"] and r["errorCode"] == "NOT_A_FONT", r)
r = c("font_coverage", {"font": str(FONTS / "Wide.otf"), "chars": "   "})
check("chars with nothing to check -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
s, dr = rpc("tools/call", {"name": "describe", "arguments": {"name": "font_coverage"}})
dd = dr["result"]["structuredContent"] if s == 200 else {}
check("font_coverage is described: read-only, every argument documented, mentions tofu and game_run",
      dd.get("ok") and dd["data"]["effect"] == "read" and all(v.get("description") for v in dd["data"]["inputSchema"]["properties"].values())
      and "tofu" in dd["data"]["description"] and "glyphMisses" in dd["data"]["description"], dd)
shutil.rmtree(FONTS, ignore_errors=True)
check("font test files removed", not FONTS.exists())

# ---------------------------------------------------------------- ER-11: loc_gather (GatherText commandlet) + loc_gather_status
GT = PROJ / "Saved" / "Enginewright" / "loc_gather_test"   # this test's own folder, removed at the end
shutil.rmtree(GT, ignore_errors=True)
(GT / "Config").mkdir(parents=True)
(GT / "Source").mkdir()
(GT / "Source" / "Probe.h").write_text('#define LOCTEXT_NAMESPACE "EWLocTest"\nFText A = LOCTEXT("Probe.Hello", "Hello from the probe");\n'
                                       'FText B = LOCTEXT("Probe.Gold", "You have {0} gold");\n#undef LOCTEXT_NAMESPACE\n', encoding="utf-8")
GTF = "Saved/Enginewright/loc_gather_test"          # the commandlet resolves these against the project folder
DEST = str(GT).replace("\\", "/") + "/Content/Localization/EWLocTest"   # absolute, for reading the results
COMMON = f"""[CommonSettings]
SourcePath={GTF}/Content/Localization/EWLocTest
DestinationPath={GTF}/Content/Localization/EWLocTest
ManifestName=EWLocTest.manifest
ArchiveName=EWLocTest.archive
PortableObjectName=EWLocTest.po
ResourceName=EWLocTest.locres
NativeCulture=en
CulturesToGenerate=en
CulturesToGenerate=tr
bUseCultureDirectory=true

"""
(GT / "Config" / "EWLocTest_Gather.ini").write_text(COMMON + f"""[GatherTextStep0]
CommandletClass=GatherTextFromSource
SearchDirectoryPaths=%LOCPROJECTROOT%{GTF}/Source
FileNameFilters=*.h
ShouldGatherFromEditorOnlyData=false

[GatherTextStep1]
CommandletClass=GenerateGatherManifest

[GatherTextStep2]
CommandletClass=GenerateGatherArchive
""", encoding="utf-8")
(GT / "Config" / "EWLocTest_Export.ini").write_text(COMMON + """[GatherTextStep0]
CommandletClass=InternationalizationExport
bExportLoc=true
POFormat=Unreal
ShouldPersistCommentsOnExport=false
ShouldAddSourceLocationsAsComments=true
""", encoding="utf-8")
(GT / "Config" / "EWLocTest_Import.ini").write_text(COMMON + """[GatherTextStep0]
CommandletClass=InternationalizationExport
bImportLoc=true
POFormat=Unreal
""", encoding="utf-8")
(GT / "Config" / "EWLocTest_Compile.ini").write_text(COMMON + """[GatherTextStep0]
CommandletClass=GenerateTextLocalizationResource
""", encoding="utf-8")
(GT / "Config" / "EWBad_Gather.ini").write_text(COMMON.replace("EWLocTest", "EWBad") + """[GatherTextStep0]
CommandletClass=NoSuchCommandletAtAll
""", encoding="utf-8")
(GT / "Config" / "EWBad_Export.ini").write_text(COMMON.replace("EWLocTest", "EWBad") + """[GatherTextStep0]
CommandletClass=InternationalizationExport
bExportLoc=true
POFormat=Unreal
""", encoding="utf-8")
CD = "Saved/Enginewright/loc_gather_test/Config"
run_dirs = []


def rt(path):
    """Text of a file the engine wrote: UTF-16 (manifest) or UTF-8, with or without BOM."""
    b = pathlib.Path(path).read_bytes()
    if b[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return b.decode("utf-16")
    return b.decode("utf-8-sig")


def lg(args, **kw):
    r = c("loc_gather", dict({"target": "EWLocTest", "configDir": CD}, **args), **kw)
    out = (r.get("data") or {}).get("out")
    if out:
        run_dirs.append(out)
    return r


def step(r, name):
    return next((s for s in (r.get("data") or {}).get("steps", []) if s.get("step") == name), {})


r = lg({"dryRun": True})
d = r.get("data") or {}
check("loc_gather dryRun: commands with -run=GatherText for gather and compile, configs listed, nothing started",
      r["ok"] and len(d.get("commands", [])) == 2 and all("-run=GatherText" in x and "-config=" in x for x in d["commands"])
      and set(d.get("configs", {})) == {"gather", "compile"} and not pathlib.Path(d["out"]).exists(), r)
r = lg({"target": "Nope"})
check("unknown target -> CONFIG_NOT_FOUND, suggests the targets found in configDir", not r["ok"] and r["errorCode"] == "CONFIG_NOT_FOUND"
      and "EWLocTest" in r.get("suggestions", []), r)
r = lg({"steps": ["gather", "polish"]})
check("unknown step -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] in ("INVALID_ARGUMENT", "INVALID_ENUM"), r)
r = lg({"steps": ["gather", "gather"]})
check("a step listed twice -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = lg({"target": "../Game"})
check("target as a path -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = lg({"timeout": 0})
check("timeout 0 -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = lg({"steps": ["check"]})
check("check alone, before any gather: no process, verdict ISSUES (manifest and files missing), run ok",
      r["ok"] and r["data"]["verdict"] == "ISSUES" and r["data"]["check"]["clean"] is False and r["data"]["check"]["manifestExists"] is False, r)

# the real commandlet, while this editor is open and holds port 3100
r = lg({"steps": ["gather", "export", "compile", "check"], "wait": 0})
rid = (r.get("data") or {}).get("id")
check("wait 0 -> STILL_RUNNING with an id (the commandlet keeps running)", r.get("errorCode") == "STILL_RUNNING" and rid, r)
b = lg({"steps": ["check"]})
check("a second loc_gather while one runs -> BUSY", b.get("errorCode") == "BUSY", b)
r = c("loc_gather_status", {"id": rid, "wait": 400}, timeout=450)
d = r.get("data") or {}
check("status waits for the result: gather, export and compile PASSED, check found the untranslated Turkish entries -> ISSUES (run ok)",
      r["ok"] and d.get("verdict") == "ISSUES" and [step(r, s).get("verdict") for s in ("gather", "export", "compile")] == ["PASSED"] * 3, r)
check("commandlet ends with exit code 0 and logs it (no MCP port clash with the open editor)",
      all(step(r, s).get("exitCode") == 0 and step(r, s).get("reportedExitCode") == 0 for s in ("gather", "export", "compile")), d.get("steps"))
log = pathlib.Path(step(r, "gather").get("log", "x"))
check("gather log exists and the commandlet did not start an Enginewright server", log.is_file() and "GatherText completed with exit code 0" in log.read_text(encoding="utf-8", errors="replace")
      and "MCP server on" not in log.read_text(encoding="utf-8", errors="replace"), str(log))
manifest = pathlib.Path(DEST) / "EWLocTest.manifest"
check("independent read-back: the manifest holds the probe texts", manifest.is_file() and "Hello from the probe" in rt(manifest) and "You have {0} gold" in rt(manifest), str(manifest))
ck = d.get("check") or {}
tr = next((x for x in ck.get("cultures", []) if x["culture"] == "tr"), {})
check("check: tr po has 2 entries, both untranslated; tr locres written; native en is not judged",
      tr.get("po", {}).get("entries") == 2 and tr["po"].get("untranslated") == 2 and tr.get("locres", {}).get("exists") is True
      and next(x for x in ck["cultures"] if x["culture"] == "en")["po"].get("untranslated") == 0 and any("untranslated" in i for i in ck["issues"]), ck)
s = c("loc_gather_status", {"id": rid})
check("status of a finished run gives the same report", s["ok"] and s["data"]["id"] == rid and s["data"]["verdict"] == "ISSUES", s)
check("summary.json written", pathlib.Path(d.get("summary", "x")).is_file(), d.get("summary"))

# fill the Turkish translations, import, compile: the check is clean and the compiled file reads back
TRPO = pathlib.Path(DEST) / "tr" / "EWLocTest.po"
txt = rt(TRPO).replace("\r\n", "\n")
txt = txt.replace('msgid "Hello from the probe"\nmsgstr ""', 'msgid "Hello from the probe"\nmsgstr "Probeden merhaba"')
txt = txt.replace('msgid "You have {0} gold"\nmsgstr ""', 'msgid "You have {0} gold"\nmsgstr "{0} altının var"')
TRPO.write_text(txt, encoding="utf-8")
r = lg({"steps": ["import", "compile", "check"], "wait": 400}, timeout=450)
d = r.get("data") or {}
check("import + compile + check after filling the po -> PASSED, check clean", r["ok"] and d.get("verdict") == "PASSED" and d["check"]["clean"] is True
      and [step(r, s).get("verdict") for s in ("import", "compile")] == ["PASSED", "PASSED"], r)
LRES = str(pathlib.Path(DEST) / "tr" / "EWLocTest.locres")
CSV2 = GT / "Source.csv"
CSV2.write_text("Key,SourceString\nProbe.Hello,Hello from the probe\nProbe.Gold,You have {0} gold\n", encoding="utf-8")
r = c("stringtable_compare", {"sourceCsv": str(CSV2), "locres": LRES, "namespace": "EWLocTest"})
check("the .locres the commandlet compiled reads back through stringtable_compare: complete, translated, hashes match (clean)",
      r["ok"] and r["data"]["translationEntries"] == 2 and r["data"]["clean"] is True and r["data"]["counts"]["sameAsSource"] == 0, r)

# a step that fails: the run stops there, later steps are skipped
r = lg({"target": "EWBad", "steps": ["gather", "export"], "wait": 300}, timeout=350)
d = r.get("data") or {}
check("a commandlet that cannot run -> RUN_NOT_PASSED, the step's verdict and exit codes reported, later steps skipped",
      not r["ok"] and r.get("errorCode") == "RUN_NOT_PASSED" and step(r, "gather").get("verdict") == "FAILED" and step(r, "gather").get("exitCode") != 0 and step(r, "gather").get("errorCount", 0) >= 1
      and d.get("skipped") == ["export"], r)

# stop a running commandlet
r = lg({"steps": ["gather"], "wait": 0})
rid = (r.get("data") or {}).get("id")
s = c("loc_gather_status", {"id": rid, "stop": True}, timeout=60)
check("stop ends the running commandlet -> verdict STOPPED", not s["ok"] and s["data"]["verdict"] == "STOPPED" and step(s, "gather").get("verdict") == "STOPPED", s)
s = c("loc_gather_status", {"id": "nope"})
check("unknown run id -> NO_RUN", s.get("errorCode") == "NO_RUN", s)
s, dr = rpc("tools/call", {"name": "describe", "arguments": {"name": "loc_gather"}})
dd = dr["result"]["structuredContent"] if s == 200 else {}
check("loc_gather description: steps, the exit-code rule, no port clash; every argument documented", dd["ok"] and "GatherText completed with exit code" in dd["data"]["description"]
      and all(v.get("description") for v in dd["data"]["inputSchema"]["properties"].values()), dd)

for out in set(run_dirs):
    shutil.rmtree(out, ignore_errors=True)
shutil.rmtree(GT, ignore_errors=True)
check("loc_gather test files removed", not GT.exists())

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
