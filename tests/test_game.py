"""game_run / game_run_status / image_compose (ER-02, ER-03): run the game from the editor and compose PNGs.

Needs a running editor on ENGINEWRIGHT_TEST_PROJECT. Most runs use a stand-in game (tests/fakegame.py, started
through game_run's exe/exeArgs) so they are fast and deterministic. Set ENGINEWRIGHT_TEST_GAME=1 to also start
the real game once (UnrealEditor <project> -game, a few minutes). Files go to the system temp folder and
<project>/Saved/Enginewright/runs.
"""
import os, pathlib, shutil, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ewpng
from ewtest import PROJ, call as c, check, done, ensure_stopped, rpc

HERE = pathlib.Path(__file__).resolve().parent
FAKE = str(HERE / "fakegame.py")
PY = sys.executable
TMP = pathlib.Path(tempfile.mkdtemp(prefix="ew_game_test_"))
ensure_stopped()


def fake(url, **kw):
    args = {"exe": PY, "exeArgs": [FAKE], "url": url}
    args.update(kw)
    return c("game_run", args, timeout=900)


def run0(r):
    return ((r.get("data") or {}).get("runs") or [{}])[0]


def gradient(path, w, h, seed):
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 4
            px[o:o + 4] = bytes(((x * 7 + seed) & 255, (y * 5 + seed * 3) & 255, (x ^ y) & 255, 255))
    ewpng.write_png(str(path), w, h, px)
    return px


# ---------------------------------------------------------------- describe / schema
def meta(tool, args):
    s, r = rpc("tools/call", {"name": tool, "arguments": args})
    return r["result"]["structuredContent"] if s == 200 else {"ok": False, "http": s, "body": r}


d = meta("describe", {"name": "game_run"})
check("game_run is described, read-only, waits", d["ok"] and d["data"]["effect"] == "read" and d["data"]["waits"]
      and "resolutions" in d["data"]["inputSchema"]["properties"], d)
d = meta("describe", {"name": "image_compose"})
check("image_compose is described with rows/columns/out", d["ok"] and {"rows", "columns", "out"} <= set(d["data"]["inputSchema"]["properties"]), d)
s = meta("search", {"query": "run game resolution screenshots"})
check("search finds game_run", s["ok"] and any(x["name"] == "game_run" for x in s["data"]["results"][:3]), s)
s = meta("search", {"query": "side by side images"})
check("search finds image_compose", s["ok"] and any(x["name"] == "image_compose" for x in s["data"]["results"][:3]), s)
V03 = ["widget_create", "widget_tree", "widget_get", "widget_add", "widget_set", "widget_remove", "widget_render", "ui_state", "ui_click",
       "ui_open", "ui_close", "ui_input_type", "ui_input_audit", "gameplay_tags_list", "gameplay_tag_add", "gameplay_tag_remove",
       "pie_actors", "pie_net_emulation", "class_measure", "screen_coverage", "asset_rename", "stringtable_list", "stringtable_get",
       "stringtable_set", "stringtable_compare", "loc_culture", "game_run", "game_run_status", "image_compose"]
undescribed = []
for n in V03:
    d = meta("describe", {"name": n})
    if not d["ok"]:
        undescribed.append(n)
        continue
    props = d["data"]["inputSchema"].get("properties", {})
    undescribed += [f"{n}.{k}" for k, v in props.items() if not v.get("description")]
    undescribed += [f"{n}.required:{k}" for k in d["data"]["inputSchema"].get("required", []) if k not in props]
check("every v0.3 tool argument has a description in its schema", not undescribed, undescribed)

# ---------------------------------------------------------------- game_run with a stand-in game
r = fake("/Game/Maps/Test?Mode=pass?Shots={shots}", resolutions=["320x200", "160x90"],
         filter=["SELFTEST", "[Save]"], **{"pass": [r"SELFTEST .*PASSED"], "fail": [r"SELFTEST .*FAILED"]})
data = r.get("data") or {}
runs = data.get("runs", [])
check("run at two resolutions -> PASSED each", r["ok"] and data.get("verdicts") == {"320x200": "PASSED", "160x90": "PASSED"}, r)
if len(runs) == 2:
    r0 = runs[0]
    check("the URL reached the game unchanged (checked in the game log)", all(x["urlConfirmed"] for x in runs), runs)
    check("kept lines: SELFTEST and [Save] only", any("[Save] slot=Test01" in l for l in r0["lines"]) and
          any("PASSED" in l for l in r0["lines"]) and not any("noise" in l for l in r0["lines"]), r0["lines"])
    shots = [x["screenshots"] for x in runs]
    check("PNG collected per run with its real size", [(s[0]["width"], s[0]["height"], s[0]["matchesRes"]) for s in shots if s]
          == [(320, 200, True), (160, 90, True)], shots)
    check("resolution passed as -ResX/-ResY", "-ResX=320" in r0["command"] and "-ResY=200" in r0["command"], r0["command"])
    check("summary.json written", pathlib.Path(data["summary"]).is_file(), data.get("summary"))
    check("collected PNG decodes (independent read-back)", ewpng.read_png(shots[0][0]["path"])[:2] == (320, 200) if shots[0] else False, shots)

r = fake("/Game/Maps/Test?Mode=fail", filter=["SELFTEST"], **{"pass": ["PASSED"], "fail": ["FAILED"]})
check("failing self-test -> RUN_NOT_PASSED, verdict FAILED with the failure line", not r["ok"] and r.get("errorCode") == "RUN_NOT_PASSED"
      and run0(r).get("verdict") == "FAILED" and any("expected 3 got 2" in l for l in run0(r).get("failLines", [])), r)

t0 = time.time()
r = fake("/Game/Maps/Test?Mode=hang", timeout=4, **{"pass": ["PASSED"]})
check("game that never ends -> TIMEOUT, stopped", run0(r).get("verdict") == "TIMEOUT" and run0(r).get("exited") is False
      and time.time() - t0 < 40, r)

r = fake("/Game/Maps/Test?Mode=passhang", done="SELFTEST DONE", doneGrace=1, timeout=60, **{"pass": ["PASSED"]})
check("passed but did not quit -> PASSED, stopped after done (reported, not hidden)", r["ok"] and run0(r).get("verdict") == "PASSED"
      and run0(r).get("stopReason") == "DONE_BUT_NOT_EXITED" and run0(r).get("seconds", 99) < 30, r)

r = fake("/Game/Maps/Test?Mode=slowdone", done="SELFTEST DONE", doneGrace=1, require="SELFTEST Logic start", requireWithin=20,
         timeout=60, **{"pass": ["PASSED"]})
check("done and require lines written after a pause are seen (first line of a new log write)", r["ok"]
      and run0(r).get("stopReason") == "DONE_BUT_NOT_EXITED" and run0(r).get("seconds", 99) < 30, r)

r = fake("/Game/Maps/Test?Mode=pass?Mangle=1", **{"pass": ["PASSED"]})
check("URL cut on the way (like a broken .bat) -> URL_MISMATCH, game stopped", run0(r).get("verdict") == "URL_MISMATCH", r)

r = fake("/Game/Maps/Test?Mode=quiet", require="SELFTEST", requireWithin=2, timeout=60)
check("test mode never engaged -> REQUIRED_LINE_MISSING, stopped early", run0(r).get("verdict") == "REQUIRED_LINE_MISSING"
      and run0(r).get("seconds", 99) < 20, r)

r = fake("/Game/Maps/Test?Mode=crash", **{"pass": ["PASSED"]})
check("crash -> CRASHED with the crash lines", run0(r).get("verdict") == "CRASHED" and run0(r).get("crashLines"), r)

r = fake("/Game/Maps/Test?Mode=pass?Code=0")
check("no patterns, exit 0 -> EXITED_OK", r["ok"] and run0(r).get("verdict") == "EXITED_OK", r)
r = fake("/Game/Maps/Test?Mode=pass?Code=5")
check("no patterns, exit 5 -> EXIT_CODE", not r["ok"] and run0(r).get("verdict") == "EXIT_CODE" and run0(r).get("exitCode") == 5, r)
r = fake("/Game/Maps/Test?Mode=nolog", urlCheckTimeout=5, **{"pass": ["PASSED"]})
check("game wrote no log -> URL_NOT_CONFIRMED + warning", run0(r).get("verdict") == "URL_NOT_CONFIRMED" and run0(r).get("warning"), r)
r = fake("/Game/Maps/Test?Mode=pass?Note=a b=c?Empty=", **{"pass": ["PASSED"]})
check("URL with spaces and '=' arrives intact", r["ok"] and run0(r).get("urlConfirmed") is True, r)

# ER-09: screenshot runs end by the number of new PNG files (the game itself never quits, like -ExecCmds=HighResShot)
t0 = time.time()
r = fake("/Game/Maps/Test?Mode=shothang?Shots={shots}?ShotCount=2", resolutions=["96x64"], shotsExpected=2, timeout=90)
check("shotsExpected reached -> PASSED, stopped (SHOTS_DONE), not by the timeout", r["ok"] and run0(r).get("verdict") == "PASSED"
      and run0(r).get("stopReason") == "SHOTS_DONE" and not run0(r).get("timedOut") and time.time() - t0 < 40, r)
check("shotsExpected run reports both PNGs, fully written", run0(r).get("shotsFound") == 2 and run0(r).get("shotsExpected") == 2
      and len(run0(r).get("screenshots", [])) == 2 and all(s.get("matchesRes") for s in run0(r).get("screenshots", [])), run0(r))
r = fake("/Game/Maps/Test?Mode=shothang?Shots={shots}?ShotCount=2", shotsExpected=3, timeout=6)
check("fewer PNGs than shotsExpected -> TIMEOUT with shotsFound", run0(r).get("verdict") == "TIMEOUT" and run0(r).get("shotsFound") == 2
      and run0(r).get("shotsExpected") == 3, r)
r = fake("/Game/Maps/Test?Mode=shothang?Shots={shots}", shotsExpected=1, timeout=60, **{"pass": ["NO SUCH LINE"]})
check("shotsExpected with pass patterns that never match -> NO_VERDICT (patterns still decide)", run0(r).get("verdict") == "NO_VERDICT"
      and run0(r).get("stopReason") == "SHOTS_DONE", r)
r = c("game_run", {"url": "/X", "shotsExpected": -1})
check("shotsExpected below 0 -> INVALID_ARGUMENT", r.get("errorCode") == "INVALID_ARGUMENT", r)

# ER-07 / ER-08: editor checks before the run (code on disk vs Live Coding, heavy editor level, realtime viewports)
r = c("game_run", {"url": "/X", "dryRun": True})
chk = (r.get("data") or {}).get("checks") or {}
scene = chk.get("editorScene") or {}
check("dryRun reports the editor checks: Live Coding patches, editor level", r["ok"] and isinstance(chk.get("liveCodingPatches"), int)
      and scene.get("level") and isinstance(scene.get("actors"), int) and "heavy" in scene and scene.get("viewportsPaused") == 0, r)
realtime_before = scene.get("realtimeViewports")
r = fake("/Game/Maps/Test?Mode=pass", **{"pass": ["PASSED"]})
sc = ((r.get("data") or {}).get("checks") or {}).get("editorScene") or {}
check("realtime level viewports are paused for the run (default editorViewports pause)", r["ok"] and sc.get("viewportsPaused") == sc.get("realtimeViewports"), sc)
r = c("game_run", {"url": "/X", "dryRun": True})
check("viewports are realtime again after the run (read back)", ((r.get("data") or {}).get("checks") or {}).get("editorScene", {}).get("realtimeViewports") == realtime_before, r)
r = fake("/Game/Maps/Test?Mode=pass", editorViewports="keep", **{"pass": ["PASSED"]})
check("editorViewports keep -> nothing paused", r["ok"] and ((r.get("data") or {}).get("checks") or {}).get("editorScene", {}).get("viewportsPaused") == 0, r)
r = c("game_run", {"url": "/X", "editorViewports": "stop", "dryRun": True})
check("editorViewports with a bad value -> INVALID_ARGUMENT", r.get("errorCode") == "INVALID_ARGUMENT", r)
d = meta("describe", {"name": "game_run"})
check("game_run description: rebuild after C++ changes, Live Coding is not enough", d["ok"] and "editor_restart" in d["data"]["description"]
      and "Live Coding patches only" in d["data"]["description"], d)

# ER-12: culture for the game process (loc_culture only changes the editor)
r = fake("/Game/Maps/Test?Mode=pass", culture="tr", **{"pass": ["PASSED"]})
check("culture tr -> -culture=tr reaches the game (checked in its own command line)", r["ok"] and "-culture=tr" in run0(r).get("commandLineSeen", "")
      and "-culture=tr" in run0(r).get("command", []) and run0(r).get("culture") == "tr", run0(r))
r = fake("/Game/Maps/Test?Mode=pass", **{"pass": ["PASSED"]})
check("no culture -> no -culture argument", r["ok"] and "-culture" not in run0(r).get("commandLineSeen", ""), run0(r))
r = c("game_run", {"url": "/X", "culture": "zz-Nowhere", "dryRun": True})
check("unknown culture -> UNKNOWN_CULTURE", r.get("errorCode") == "UNKNOWN_CULTURE", r)
r = c("game_run", {"url": "/X", "culture": " ", "dryRun": True})
check("empty culture -> INVALID_ARGUMENT", r.get("errorCode") == "INVALID_ARGUMENT", r)
r = c("game_run", {"url": "/X", "culture": "tr", "args": ["-culture=de"], "dryRun": True})
check("culture together with -culture= in args -> INVALID_ARGUMENT", r.get("errorCode") == "INVALID_ARGUMENT", r)
r = c("game_run", {"url": "/X", "culture": "de-DE", "dryRun": True})
check("dryRun shows -culture=de-DE in the command", r["ok"] and "-culture=de-DE" in run0(r).get("command", []), r)
d = meta("describe", {"name": "game_run"})
check("game_run description + schema: culture, and that loc_culture does not reach the game", d["ok"] and "culture" in d["data"]["inputSchema"]["properties"]
      and "loc_culture" in d["data"]["description"], d)
d = meta("describe", {"name": "loc_culture"})
check("loc_culture description says it does not reach game_run", d["ok"] and "game_run" in d["data"]["description"], d)

# ER-13: glyph misses (tofu) counted from the game's log
r = fake("/Game/Maps/Test?Mode=pass?Glyph=1", **{"pass": ["PASSED"]})
g = run0(r)
check("glyphMisses counts the Slate 'Could not find Glyph Index' lines, with the distinct code points", r["ok"] and g.get("glyphMisses") == 3
      and g.get("glyphMissCodePoints") == ["U+0130", "U+011E"] and "Roboto-Regular" in g.get("glyphMissSample", ""), g)
check("glyph misses are also a warning that names font_coverage", any("font_coverage" in w and "U+0130" in w for w in r.get("warnings", [])), r.get("warnings"))
r = fake("/Game/Maps/Test?Mode=pass", **{"pass": ["PASSED"]})
check("a run without glyph warnings reports glyphMisses 0 and no code points", run0(r).get("glyphMisses") == 0 and "glyphMissCodePoints" not in run0(r), run0(r))
d = meta("describe", {"name": "game_run"})
check("game_run description mentions glyphMisses", d["ok"] and "glyphMisses" in d["data"]["description"], d)

# background: reply at once, follow with game_run_status
r = fake("/Game/Maps/Test?Mode=pass", wait=0, **{"pass": ["PASSED"]})
rid = (r.get("data") or {}).get("id")
check("wait 0 -> STILL_RUNNING with an id (the game keeps running)", r.get("errorCode") == "STILL_RUNNING" and rid, r)
b = fake("/Game/Maps/Test?Mode=pass")
check("a second game_run while one runs -> BUSY", b.get("errorCode") == "BUSY", b)
s = c("game_run_status", {"id": rid, "wait": 60}, timeout=120)
check("game_run_status waits for the result -> PASSED", s["ok"] and (s.get("data") or {}).get("verdicts") == {"default": "PASSED"}, s)
s = c("game_run_status", {})
check("game_run_status without id -> the latest run", s["ok"] and s["data"]["id"] == rid, s)
r = fake("/Game/Maps/Test?Mode=hang", wait=0, timeout=300)
rid = (r.get("data") or {}).get("id")
time.sleep(2)
s = c("game_run_status", {"id": rid, "stop": True})
check("stop ends a running game -> verdict STOPPED", not s["ok"] and run0(s).get("verdict") == "STOPPED" and run0(s).get("exited") is False, s)
s = c("game_run_status", {"id": "nope"})
check("unknown run id -> NO_RUN", s.get("errorCode") == "NO_RUN", s)

# argument checks
r = c("game_run", {"url": "/X", "resolutions": ["big"]})
check("bad resolution -> INVALID_ARGUMENT", r.get("errorCode") == "INVALID_ARGUMENT" and "1920x1080" in r.get("message", ""), r)
r = c("game_run", {"url": "/X?Dir={shot}", "dryRun": True})
check("unknown placeholder -> INVALID_ARGUMENT naming the known ones", r.get("errorCode") == "INVALID_ARGUMENT" and "{shot}" in r.get("message", "")
      and "{shots}" in r.get("message", ""), r)
r = c("game_run", {"exe": "C:/no/such/game.exe"})
check("missing exe -> NOT_FOUND", r.get("errorCode") == "NOT_FOUND", r)
r = c("game_run", {"url": "/X", "resolution": "1280x720"})
check("misspelled argument -> UNKNOWN_ARGUMENT with suggestion", r.get("errorCode") == "UNKNOWN_ARGUMENT" and "resolutions" in r.get("suggestions", []), r)
outdir = TMP / "dry"
r = c("game_run", {"url": "/X?Shots={shots}", "resolutions": ["640x360"], "dryRun": True, "out": str(outdir)})
cmd = (run0(r).get("command") or [])
check("dryRun returns the editor -game command, starts nothing, creates no folder", r["ok"] and run0(r)["url"].endswith("/640x360/shots")
      and "-game" in cmd and any(a.lower().endswith(".uproject") for a in cmd) and "verdict" not in run0(r) and not outdir.exists(), r)

# ---------------------------------------------------------------- image_compose
a = gradient(TMP / "a.png", 120, 80, 1)
b = gradient(TMP / "b.png", 120, 80, 50)
c_ = gradient(TMP / "c.png", 90, 60, 100)
d_ = gradient(TMP / "d.png", 90, 60, 150)
out = TMP / "cmp.png"
r = c("image_compose", {"out": str(out), "columns": ["Web", "Oyun"], "title": "Karşılaştırma ğüşİıöç", "cellWidth": 0,
                        "rows": [{"label": "1920", "images": [str(TMP / "a.png"), str(TMP / "b.png")]},
                                 {"label": "1280", "images": [str(TMP / "c.png"), str(TMP / "d.png")]}]})
check("compose 2x2 with column/row labels and title -> ok", r["ok"] and len(r["data"].get("cells", [])) == 4, r)
if r["ok"]:
    W, H, px = ewpng.read_png(str(out))
    check("output file decodes and has the reported size", [W, H] == r["data"]["size"], [W, H, r["data"]["size"]])
    exact = True
    for cell, src in zip(r["data"]["cells"], (a, b, c_, d_)):
        x0, y0, cw, chh = cell["rect"]
        for yy in (0, chh // 2, chh - 1):
            if px[((y0 + yy) * W + x0) * 4:((y0 + yy) * W + x0 + cw) * 4] != src[yy * cw * 4:(yy + 1) * cw * 4]:
                exact = False
    check("each cell holds its source image pixel for pixel (independent read-back)", exact, r["data"]["cells"])
    check("Turkish label characters are all in the font", r["data"]["unsupportedChars"] == "", r["data"]["unsupportedChars"])
    check("cells keep row/column labels and source sizes", r["data"]["cells"][0]["colLabel"] == "Web" and r["data"]["cells"][3]["rowLabel"] == "1280"
          and r["data"]["cells"][2]["sourceSize"] == [90, 60], r["data"]["cells"])
    band = px[(4 * W + 4) * 4:(4 * W + 4) * 4 + 3]
    check("title band drawn (dark band colour at the top)", bytes(band) == bytes((40, 40, 48)), list(band))
r = c("image_compose", {"out": str(TMP / "two.png"), "images": [str(TMP / "a.png"), str(TMP / "c.png")], "cellWidth": 60})
check("one row, scaled to cellWidth", r["ok"] and all(x["rect"][2] == 60 for x in r["data"]["cells"]), r)
r = c("image_compose", {"out": str(TMP / "sym.png"), "images": [str(TMP / "a.png")], "title": "日本 ✓"})
check("characters outside the font -> listed + warning, still written", r["ok"] and r["data"]["unsupportedChars"] and r.get("warnings"), r)
r = c("image_compose", {"out": str(TMP / "x.png"), "images": [str(TMP / "a.png"), str(TMP / "missing.png")]})
check("missing image -> NOT_FOUND", r.get("errorCode") == "NOT_FOUND", r)
(TMP / "bad.png").write_bytes(b"not a png at all, just text")
r = c("image_compose", {"out": str(TMP / "x.png"), "images": [str(TMP / "a.png"), str(TMP / "bad.png")]})
check("non-PNG file -> BAD_IMAGE", r.get("errorCode") == "BAD_IMAGE", r)
r = c("image_compose", {"out": str(TMP / "x.png"), "columns": ["A", "B", "C"], "images": [str(TMP / "a.png"), str(TMP / "b.png")]})
check("column label count must match -> INVALID_ARGUMENT", r.get("errorCode") == "INVALID_ARGUMENT", r)
r = c("image_compose", {"out": str(TMP / "x.png"), "rows": [{"label": "only-label"}]})
check("row without images -> INVALID_ARGUMENT", r.get("errorCode") == "INVALID_ARGUMENT", r)
r = c("image_compose", {"out": str(TMP / "x.png"), "rows": [{"lable": "x", "images": [str(TMP / "a.png")]}]})
check("unknown key in a row -> UNKNOWN_ARGUMENT", r.get("errorCode") == "UNKNOWN_ARGUMENT", r)
r = c("image_compose", {"out": str(TMP / "x.png")})
check("no rows/images -> MISSING_ARGUMENT", r.get("errorCode") == "MISSING_ARGUMENT", r)
r = c("image_compose", {"out": str(TMP / "x.jpg"), "images": [str(TMP / "a.png")]})
check("out must be .png -> INVALID_ARGUMENT", r.get("errorCode") == "INVALID_ARGUMENT", r)
check("failed compose writes no output", not (TMP / "x.png").exists())

# ---------------------------------------------------------------- the real game (optional)
if os.environ.get("ENGINEWRIGHT_TEST_GAME") == "1":
    r = c("game_run", {"url": "/Engine/Maps/Templates/Template_Default?EWProbe=1", "resolutions": ["1280x720"],
                       "args": ["-ExecCmds=HighResShot 1"], "pass": ["High resolution screenshot saved"], "done": "High resolution screenshot saved",
                       "doneGrace": 5, "filter": ["Screenshot"], "timeout": 900, "wait": 900}, timeout=1000)
    check("real game: URL confirmed in its log", run0(r).get("urlConfirmed") is True, r)
    pngs = run0(r).get("screenshots", [])
    check("real game: HighResShot PNG collected at 1280x720", any(s.get("width") == 1280 and s.get("height") == 720 for s in pngs), r)
    check("real game: PASSED and stopped by done, not by the timeout", run0(r).get("verdict") == "PASSED"
          and run0(r).get("stopReason") == "DONE_BUT_NOT_EXITED" and not run0(r).get("timedOut"), r)
    # ER-09: the same screenshot run without any log pattern, ended by the PNG count
    r = c("game_run", {"url": "/Engine/Maps/Templates/Template_Default?EWProbe=2", "resolutions": ["960x540"],
                       "args": ["-ExecCmds=HighResShot 1"], "shotsExpected": 1, "timeout": 900, "wait": 900}, timeout=1000)
    check("real game: HighResShot with shotsExpected 1 -> PASSED (SHOTS_DONE), not TIMEOUT", r["ok"] and run0(r).get("verdict") == "PASSED"
          and run0(r).get("stopReason") == "SHOTS_DONE" and any(s.get("width") == 960 for s in run0(r).get("screenshots", [])), r)
else:
    print("SKIP real game run (set ENGINEWRIGHT_TEST_GAME=1)")

shutil.rmtree(TMP, ignore_errors=True)
done()
