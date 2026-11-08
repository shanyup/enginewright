"""Tests for Plugins/Enginewright/Scripts/ewtool.py (build / status / locks / run / test / compose).

Runs without the editor: game runs use a stand-in game (tests/ewtool_fakegame.py). The build checks use
UnrealBuildTool on ENGINEWRIGHT_TEST_PROJECT (it needs a C++ editor target). If the project's editor is open,
the build is expected to be refused with the editor named as the lock holder; otherwise the suite builds.
Set ENGINEWRIGHT_TEST_GAME=1 to also start the real game once (UnrealEditor -game, a few minutes).
Everything is written under <project>/Saved/Enginewright/ and the system temp folder.
"""
import json, os, pathlib, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ewtest import PROJ, check, done

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent / "Plugins" / "Enginewright" / "Scripts"
TOOL = SCRIPTS / "ewtool.py"
FAKE = HERE / "ewtool_fakegame.py"
sys.path.insert(0, str(SCRIPTS))
import ew_png  # noqa: E402

UPROJECT = next(PROJ.glob("*.uproject"))
TMP = pathlib.Path(tempfile.mkdtemp(prefix="ewtool_test_"))


def ew(*args, env=None, timeout=1800):
    e = dict(os.environ, **(env or {}))
    p = subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, encoding="utf-8",
                       errors="replace", env=e, timeout=timeout)
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        return p.returncode, {"raw": p.stdout[-2000:], "stderr": p.stderr[-2000:]}


def fake(url, *extra):
    return ew("run", "--project", str(UPROJECT), "--exe", sys.executable, "--exe-arg", str(FAKE), "--url", url, *extra)


def gradient(path, w, h, seed):
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 4
            px[o:o + 4] = bytes(((x * 7 + seed) & 255, (y * 5 + seed * 3) & 255, (x ^ y) & 255, 255))
    ew_png.write_png(str(path), w, h, px)
    return px


# ---------------------------------------------------------------- compose
a = gradient(TMP / "a.png", 120, 80, 1)
b = gradient(TMP / "b.png", 120, 80, 50)
c_ = gradient(TMP / "c.png", 90, 60, 100)
d = gradient(TMP / "d.png", 90, 60, 150)
out = TMP / "cmp.png"
code, r = ew("compose", "--out", str(out), "--cols", "Web,Oyun", "--title", "Karşılaştırma ğüşİıöç", "--cell-width", "0",
             "--row", "1920", str(TMP / "a.png"), str(TMP / "b.png"), "--row", "1280", str(TMP / "c.png"), str(TMP / "d.png"))
check("compose 2x2 with column/row labels and title -> ok", code == 0 and r.get("ok") and len(r.get("cells", [])) == 4, r)
if r.get("ok"):
    W, H, px = ew_png.read_png(str(out))
    check("output file decodes and has the reported size", [W, H] == r["size"], [W, H, r["size"]])
    exact = True
    for cell, src in zip(r["cells"], (a, b, c_, d)):
        x0, y0, cw, chh = cell["rect"]
        for yy in (0, chh // 2, chh - 1):
            if px[((y0 + yy) * W + x0) * 4:((y0 + yy) * W + x0 + cw) * 4] != src[yy * cw * 4:(yy + 1) * cw * 4]:
                exact = False
    check("each cell holds its source image pixel for pixel (independent read-back)", exact, r["cells"])
    check("Turkish label characters are all in the font", r["unsupportedChars"] == "", r["unsupportedChars"])
    check("cells keep row/column labels and source sizes",
          r["cells"][0]["colLabel"] == "Web" and r["cells"][3]["rowLabel"] == "1280" and r["cells"][2]["sourceSize"] == [90, 60], r["cells"])

code, r2 = ew("compose", "--out", str(TMP / "pure.png"), "--cols", "Web,Oyun", "--title", "Karşılaştırma ğüşİıöç", "--cell-width", "0",
              "--row", "1920", str(TMP / "a.png"), str(TMP / "b.png"), "--row", "1280", str(TMP / "c.png"), str(TMP / "d.png"),
              env={"EW_NO_PILLOW": "1"})
check("compose without Pillow (pure Python decoder) gives the same image",
      code == 0 and r2.get("decoder") == "python" and ew_png.read_png(str(TMP / "pure.png"))[2] == ew_png.read_png(str(out))[2], r2)

code, r = ew("compose", "--out", str(TMP / "two.png"), str(TMP / "a.png"), str(TMP / "c.png"), "--cell-width", "60")
check("compose one row, scaled to --cell-width", code == 0 and r.get("ok") and all(c["rect"][2] == 60 for c in r["cells"]), r)
code, r = ew("compose", "--out", str(TMP / "x.png"), str(TMP / "a.png"), str(TMP / "missing.png"))
check("missing image -> NOT_FOUND, exit 2", code == 2 and r.get("error") == "NOT_FOUND", r)
(TMP / "bad.png").write_bytes(b"not a png at all, just text")
code, r = ew("compose", "--out", str(TMP / "x.png"), str(TMP / "a.png"), str(TMP / "bad.png"))
check("non-PNG file -> BAD_IMAGE", code == 2 and r.get("error") == "BAD_IMAGE", r)
code, r = ew("compose", "--out", str(TMP / "x.png"), "--cols", "A,B,C", str(TMP / "a.png"), str(TMP / "b.png"))
check("column label count must match -> BAD_IMAGE", code == 2 and r.get("error") == "BAD_IMAGE", r)
code, r = ew("compose", "--out", str(TMP / "x.png"), "--row", "only-label")
check("--row without images -> BAD_ARGS", code == 2 and r.get("error") == "BAD_ARGS", r)
check("failed compose writes no output", not (TMP / "x.png").exists())

try:
    from PIL import Image
    Image.new("P", (40, 30), 7).save(TMP / "pal.png")
    Image.new("LA", (40, 30), (90, 200)).save(TMP / "la.png")
    im = Image.frombytes("RGB", (64, 48), bytes((i * 37) & 255 for i in range(64 * 48 * 3)))
    im.save(TMP / "noise.png", optimize=True)  # exercises Sub/Up/Average/Paeth filters
    same = all(ew_png._read_png_pure(str(TMP / f))[2] == bytearray(Image.open(TMP / f).convert("RGBA").tobytes())
               for f in ("pal.png", "la.png", "noise.png"))
    check("pure Python decoder matches Pillow (palette, gray+alpha, filtered RGB)", same)
except ImportError:
    print("SKIP decoder vs Pillow comparison (Pillow not installed)")

# ---------------------------------------------------------------- run / test with a stand-in game
code, r = fake("/Game/Maps/Test?Mode=pass?Shots={shots}", "--res", "320x200", "--res", "160x90",
               "--filter", "SELFTEST", "--filter", "[Save]", "--pass", r"SELFTEST .*PASSED", "--fail", r"SELFTEST .*FAILED")
runs = r.get("runs", [])
check("run at two resolutions -> PASSED each, exit 0", code == 0 and r.get("verdicts") == {"320x200": "PASSED", "160x90": "PASSED"}, r)
if len(runs) == 2:
    r0 = runs[0]
    check("the URL reached the game unchanged (checked in the game log)", all(x["urlConfirmed"] for x in runs), runs)
    check("kept lines: SELFTEST and [Save] only", any("[Save] slot=Test01" in l for l in r0["lines"]) and
          any("PASSED" in l for l in r0["lines"]) and not any("noise" in l for l in r0["lines"]), r0["lines"])
    shots = [x["screenshots"] for x in runs]
    check("PNG collected per run with its real size", [(s[0]["width"], s[0]["height"], s[0]["matchesRes"]) for s in shots if s]
          == [(320, 200, True), (160, 90, True)], shots)
    check("resolution passed as -ResX/-ResY", "-ResX=320" in r0["command"] and "-ResY=200" in r0["command"], r0["command"])
    check("summary.json written", pathlib.Path(r["summary"]).is_file(), r.get("summary"))

code, r = fake("/Game/Maps/Test?Mode=fail", "--filter", "SELFTEST", "--pass", "PASSED", "--fail", "FAILED")
run0 = (r.get("runs") or [{}])[0]
check("failing self-test -> FAILED with the failure line, exit 1", code == 1 and run0.get("verdict") == "FAILED"
      and any("expected 3 got 2" in l for l in run0.get("failLines", [])), r)

t0 = time.time()
code, r = fake("/Game/Maps/Test?Mode=hang", "--pass", "PASSED", "--timeout", "4")
run0 = (r.get("runs") or [{}])[0]
check("game that never ends -> TIMEOUT, stopped", code == 1 and run0.get("verdict") == "TIMEOUT" and run0.get("exited") is False
      and time.time() - t0 < 40, r)

code, r = fake("/Game/Maps/Test?Mode=passhang", "--pass", "PASSED", "--done", "SELFTEST DONE", "--done-grace", "1", "--timeout", "60")
run0 = (r.get("runs") or [{}])[0]
check("passed but did not quit -> PASSED, stopped after --done (reported, not hidden)", code == 0 and run0.get("verdict") == "PASSED"
      and run0.get("stopReason") == "DONE_BUT_NOT_EXITED" and run0.get("seconds", 99) < 30, r)

code, r = fake("/Game/Maps/Test?Mode=pass?Mangle=1", "--pass", "PASSED")
run0 = (r.get("runs") or [{}])[0]
check("URL cut on the way (like a broken .bat) -> URL_MISMATCH, game stopped", code == 1 and run0.get("verdict") == "URL_MISMATCH", r)

code, r = fake("/Game/Maps/Test?Mode=quiet", "--require", "SELFTEST", "--require-within", "2", "--timeout", "60")
run0 = (r.get("runs") or [{}])[0]
check("test mode never engaged -> REQUIRED_LINE_MISSING, stopped early", code == 1 and run0.get("verdict") == "REQUIRED_LINE_MISSING"
      and run0.get("seconds", 99) < 20, r)

code, r = fake("/Game/Maps/Test?Mode=crash", "--pass", "PASSED")
run0 = (r.get("runs") or [{}])[0]
check("crash -> CRASHED with the crash lines", code == 1 and run0.get("verdict") == "CRASHED" and run0.get("crashLines"), r)

code, r = fake("/Game/Maps/Test?Mode=pass?Code=0")
check("no patterns, exit 0 -> EXITED_OK", code == 0 and (r.get("runs") or [{}])[0].get("verdict") == "EXITED_OK", r)
code, r = fake("/Game/Maps/Test?Mode=pass?Code=5")
check("no patterns, exit 5 -> EXIT_CODE", code == 1 and (r.get("runs") or [{}])[0].get("verdict") == "EXIT_CODE", r)
code, r = fake("/Game/Maps/Test?Mode=nolog", "--pass", "PASSED", "--url-check-timeout", "5")
check("game wrote no log -> URL_NOT_CONFIRMED", code == 1 and (r.get("runs") or [{}])[0].get("verdict") == "URL_NOT_CONFIRMED", r)
code, r = fake("/Game/Maps/Test?Mode=pass?Note=a b=c?Empty=", "--pass", "PASSED")
check("URL with spaces and '=' arrives intact", code == 0 and (r.get("runs") or [{}])[0].get("urlConfirmed") is True, r)

code, r = ew("test", "--project", str(UPROJECT), "--exe", sys.executable, "--exe-arg", str(FAKE), "--url", "/X?Mode=pass", "--no-build")
check("test without --pass/--fail -> BAD_ARGS", code == 2 and r.get("error") == "BAD_ARGS", r)
code, r = fake("/X?Mode=pass", "--res", "big")
check("bad resolution -> BAD_ARGS", code == 2 and r.get("error") == "BAD_ARGS", r)
code, r = fake("/X?Mode=pass?Dir={shot}")
check("unknown placeholder -> BAD_ARGS naming the known ones", code == 2 and r.get("error") == "BAD_ARGS" and "{shot}" in r.get("message", ""), r)
outdir = TMP / "dry"
code, r = fake("/X?Mode=pass?Shots={shots}", "--res", "640x360", "--dry-run", "--out", str(outdir))
check("--dry-run prints the command, starts nothing, creates no folder", code == 0 and r["runs"][0]["url"].endswith("/640x360/shots")
      and "verdict" not in r["runs"][0] and not outdir.exists(), r)

# ---------------------------------------------------------------- locks and build (UnrealBuildTool)
code, r = ew("locks", "--project", str(UPROJECT))
check("locks -> report", code == 0 and r.get("ok") and r.get("filesChecked", 0) > 0, r)
editor_open = bool(r.get("locked")) and any("unrealeditor" in h["name"].lower() for h in r.get("holders", []))
if editor_open:
    code, r = ew("build", "--project", str(UPROJECT), "--lock-wait", "0")
    check("editor open -> build refused LOCKED, editor named", code == 2 and r.get("error") == "LOCKED"
          and any("unrealeditor" in h["name"].lower() and h.get("files") for h in r.get("holders", [])), r)
    print("NOTE editor is open: build/status/test-with-build checks skipped (close the editor to run them)")
else:
    dll = next((f for f in (PROJ / "Binaries").rglob("*.dll")), None)
    if dll:
        holder = subprocess.Popen([sys.executable, "-c", f"f=open(r'{dll}','rb'); import time; time.sleep(120)"])
        time.sleep(1.5)
        code, r = ew("build", "--project", str(UPROJECT), "--lock-wait", "0")
        hs = r.get("holders", [])
        # (a venv python.exe is a launcher: the file may be held by its child, so match the command line too)
        check("binary held by another process -> LOCKED naming that process and file", code == 2 and r.get("error") == "LOCKED"
              and any((h["pid"] == holder.pid or "time.sleep(120)" in h.get("commandLine", "")) and any(dll.name in f for f in h.get("files", [])) for h in hs), r)
        holder.kill()
        holder.wait()
    t0 = time.time()
    code, r = ew("build", "--project", str(UPROJECT), "--background")
    bid = r.get("id")
    check("build --background returns at once with an id", code in (0, 3) and bid and time.time() - t0 < 60, r)
    code, r = ew("status", "--project", str(UPROJECT), "--id", str(bid), "--wait", "1500")
    check("status follows the detached build to the end -> succeeded", code == 0 and r.get("status") == "succeeded"
          and r.get("result", "").startswith("Succeeded"), r)
    code, r = ew("test", "--project", str(UPROJECT), "--exe", sys.executable, "--exe-arg", str(FAKE), "--url", "/X?Mode=pass",
                 "--pass", "PASSED", "--fail", "FAILED", "--filter", "SELFTEST")
    check("test: build (up to date) -> run -> PASSED", code == 0 and r.get("build", {}).get("status") == "succeeded"
          and r.get("verdicts") == {"default": "PASSED"}, r)
code, r = ew("status", "--project", str(UPROJECT), "--id", "nope")
check("status of an unknown build -> NO_BUILD", code == 2 and r.get("error") == "NO_BUILD", r)

# ---------------------------------------------------------------- the real game (optional)
if os.environ.get("ENGINEWRIGHT_TEST_GAME") == "1":
    code, r = ew("run", "--project", str(UPROJECT), "--url", "/Engine/Maps/Templates/Template_Default?EWProbe=1",
                 "--res", "1280x720", "--arg", "-ExecCmds=HighResShot 1", "--done", "Screenshot", "--done-grace", "5",
                 "--filter", "Screenshot", "--timeout", "900")
    run0 = (r.get("runs") or [{}])[0]
    check("real game: URL confirmed in its log", run0.get("urlConfirmed") is True, r)
    pngs = run0.get("screenshots", [])
    check("real game: HighResShot PNG collected at 1280x720", any(s.get("width") == 1280 and s.get("height") == 720 for s in pngs), r)
else:
    print("SKIP real game run (set ENGINEWRIGHT_TEST_GAME=1)")

shutil.rmtree(TMP, ignore_errors=True)
done()
