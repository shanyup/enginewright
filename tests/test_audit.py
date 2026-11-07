"""Audit tool tests: text search in content, ID integrity, hard-coded key audit, A/B screenshot.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. Uses the DataTable and Blueprint made by test_core.py
(/Game/_EWTest/DT_EW with rows Apple, ApplePie; struct field ProcessedItemID).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import ROOT, call as c, check, done, ensure_stopped

ensure_stopped()
DT = ROOT + "/DT_EW"
if not c("asset_exists", {"path": DT})["data"]["exists"]:
    print("SKIP: run test_core.py first (needs " + DT + ")")
    done()
    sys.exit(0)

c("datatable_set_rows", {"path": DT, "rows": {"ApplePie": {"ProcessedItemID": "Aple"}}})
r = c("asset_search_text", {"text": "Aple", "folder": ROOT, "exact": True})
check("asset_search_text finds a value in a DataTable row", r["ok"] and any(h["asset"].endswith("DT_EW") and h["field"] == "ProcessedItemID" for h in r["data"]["hits"]), r)
r = c("asset_search_text", {"text": "  "})
check("empty text -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)

r = c("id_audit", {"source": DT, "folder": ROOT})
check("id_audit flags the undefined ID with a suggestion", not r["ok"] and r["errorCode"] == "UNDEFINED_IDS"
      and any(u["id"] == "Aple" and u.get("didYouMean") == "Apple" for u in r["data"]["undefined"]), r)
c("datatable_set_rows", {"path": DT, "rows": {"ApplePie": {"ProcessedItemID": "Apple"}}})
r = c("id_audit", {"source": DT, "folder": ROOT})
check("id_audit clean after the fix", r["ok"] and r["data"]["undefined"] == [], r)
r = c("id_audit", {"folder": ROOT})
check("id_audit without source or ids -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("id_audit", {"source": ROOT + "/DT_Missing"})
check("missing source table -> TABLE_NOT_FOUND", not r["ok"] and r["errorCode"] == "TABLE_NOT_FOUND", r)

r = c("key_audit", {"folder": ROOT})
check("key_audit runs and reports code + blueprints", "code" in r["data"] and "blueprints" in r["data"], r)

r = c("screenshot_ab", {"source": "editor", "command": "r.ScreenPercentage 100", "wait": 0.2, "maxWidth": 320})
check("screenshot_ab returns a side-by-side picture", r["ok"] and os.path.isfile(r["data"]["file"]) and r["data"]["width"] > 320, r)
r = c("screenshot_ab", {"source": "editor"})
check("screenshot_ab without a change -> INVALID_ARGUMENT", not r["ok"] and r["errorCode"] == "INVALID_ARGUMENT", r)
r = c("screenshot_ab", {"source": "editor", "steps": [{"tool": "object_set", "args": {"path": ROOT + "/Nope", "values": {}}}]})
check("failing step -> STEP_FAILED, no B picture", not r["ok"] and r["errorCode"] == "STEP_FAILED", r)

done()
