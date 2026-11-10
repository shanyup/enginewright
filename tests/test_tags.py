"""Gameplay Tag tests: list, add (ini), remove, and their refusals. Uses tags under EWTest only.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT (writes its Config/DefaultGameplayTags.ini).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import PROJ, call as c, check, done, ensure_stopped

ensure_stopped()


def ew_tags():
    r = c("gameplay_tags_list", {"prefix": "EWTest"})
    return [t["tag"] for t in r["data"]["tags"]] if r["ok"] else []


# cleanup from earlier runs: children first
for t in sorted(ew_tags(), key=lambda s: -s.count(".")):
    c("gameplay_tag_remove", {"tag": t})

r = c("gameplay_tag_add", {"tag": "EWTest.Item.Apple", "comment": "test tag"})
check("add tag, read back with comment and source", r["ok"] and r["data"]["tag"] == "EWTest.Item.Apple" and r["data"].get("comment") == "test tag"
      and any("DefaultGameplayTags" in s for s in r["data"].get("sources", [])), r)
r = c("gameplay_tag_add", {"tag": "EWTest.Item.Apple"})
check("add existing -> TAG_EXISTS", not r["ok"] and r["errorCode"] == "TAG_EXISTS", r)
r = c("gameplay_tag_add", {"tag": "EWTest.Bad Tag!"})
check("tag with space and symbol -> INVALID_TAG with a cleaned suggestion", not r["ok"] and r["errorCode"] == "INVALID_TAG" and "EWTest.Bad_Tag_" in r.get("suggestions", []), r)
r = c("gameplay_tag_add", {"tag": "EWTest..Double"})
check("empty tag part -> INVALID_TAG", not r["ok"] and r["errorCode"] == "INVALID_TAG", r)
r = c("gameplay_tag_add", {"tag": "EWTest.Fruit"})
r2 = c("gameplay_tag_add", {"tag": "EWTest.Fruit.Pear"})
check("add parent and child", r["ok"] and r2["ok"], [r, r2])

r = c("gameplay_tags_list", {"prefix": "EWTest"})
names = [t["tag"] for t in r["data"]["tags"]] if r["ok"] else []
check("list by prefix includes implicit parents", r["ok"] and {"EWTest.Item", "EWTest.Item.Apple", "EWTest.Fruit.Pear"} <= set(names), names)
r = c("gameplay_tags_list", {"prefix": "EWTest.Itme"})
check("unknown prefix -> TAG_NOT_FOUND with suggestion", not r["ok"] and r["errorCode"] == "TAG_NOT_FOUND" and r.get("suggestions"), r)

r = c("gameplay_tag_remove", {"tag": "EWTest.Item"})
check("remove implicit tag -> TAG_IMPLICIT", not r["ok"] and r["errorCode"] == "TAG_IMPLICIT", r)
r = c("gameplay_tag_remove", {"tag": "EWTest.Fruit"})
check("remove tag with children -> TAG_HAS_CHILDREN", not r["ok"] and r["errorCode"] == "TAG_HAS_CHILDREN", r)
r = c("gameplay_tag_remove", {"tag": "EWTest.Nope"})
check("remove unknown -> TAG_NOT_FOUND", not r["ok"] and r["errorCode"] == "TAG_NOT_FOUND", r)
for t in ("EWTest.Fruit.Pear", "EWTest.Fruit", "EWTest.Item.Apple"):
    r = c("gameplay_tag_remove", {"tag": t})
    check(f"remove {t}", r["ok"] and r["data"]["removed"] == t, r)

# --- a tag ini created by gameplay_tag_add is deleted again with its last tag
tmp_ini = PROJ / "Config/Tags/EWTestTmpTags.ini"
if tmp_ini.exists():
    tmp_ini.unlink()
r = c("gameplay_tag_add", {"tag": "EWTest.Tmp.One", "source": "EWTestTmpTags.ini"})
check("add tag into a new ini -> createdFile", r["ok"] and (r["data"].get("createdFile") or "").replace("\\", "/").endswith("Config/Tags/EWTestTmpTags.ini"), r)
check("the tag ini exists after the add", tmp_ini.is_file(), str(tmp_ini))
r = c("gameplay_tag_remove", {"tag": "EWTest.Tmp.One"})
check("removing the last tag deletes the ini it created", r["ok"]
      and (r["data"].get("removedFile") or "").replace("\\", "/").endswith("Config/Tags/EWTestTmpTags.ini") and not tmp_ini.is_file(), r)
check("no EWTest tags left", ew_tags() == [], ew_tags())

done()
