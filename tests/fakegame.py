"""A stand-in for a game, used by test_game.py (game_run) instead of a real game.

Called as: python fakegame.py <url> [-ResX=W -ResY=H ...] -abslog=<file> ...
The URL options choose what it does: Mode=pass|fail|hang|crash|passhang|shothang|slowdone|nolog|quiet, Shots=<dir> (writes ShotCount PNGs, default 1,
of the requested size there), Mangle=1 (logs a command line with the URL cut at the first '=', like a broken
batch file), Code=<n> (exit code), Glyph=1 (logs Slate 'Could not find Glyph Index' warnings).
"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ewpng  # noqa: E402

args = sys.argv[1:]
url = next((a for a in args if not a.startswith("-")), "")
opt = dict(p.split("=", 1) for p in url.split("?")[1:] if "=" in p)
val = lambda k: next((a.split("=", 1)[1] for a in args if a.startswith(f"-{k}=")), None)
log_path = val("abslog")
mode = opt.get("Mode", "pass")
if mode == "nolog" or not log_path:
    time.sleep(1)
    sys.exit(int(opt.get("Code", 0)))
log = open(log_path, "w", encoding="utf-8")


def say(line):
    log.write(line + "\n")
    log.flush()


shown = " ".join(args)
if opt.get("Mangle"):
    shown = shown.split("=", 1)[0]
say("LogInit: ExecutableName: fakegame")
say("LogInit: Command Line: " + shown)
say("LogTemp: Display: noise line")
time.sleep(0.5)
if mode == "quiet":
    time.sleep(30)
    sys.exit(0)
say("SELFTEST Logic start")
say("[Save] slot=Test01 written")
if opt.get("Glyph"):
    for cp in ("0130", "011E", "0130"):
        say(f"LogSlate: Warning: Could not find Glyph Index for character U+{cp} in font Roboto-Regular")
if opt.get("Shots"):
    w, h = int(val("ResX") or 64), int(val("ResY") or 32)
    os.makedirs(opt["Shots"], exist_ok=True)
    # ER-19: brightness follows an "r.ExposureOffset <stops>" command of -ExecCmds, 20 grey levels per stop
    pixel = b"\x10\x80\xf0\xff"
    m = re.search(r"r\.ExposureOffset\s+(-?[0-9.]+)", val("ExecCmds") or "")
    if m:
        g = max(0, min(255, int(round(128 + 20 * float(m.group(1))))))
        pixel = bytes((g, g, g, 255))
    # ER-20: ShotNames=a,b,c writes <name>_<w>x<h>.png one after the other, ShotDelay seconds apart
    if opt.get("ShotNames"):
        for i, nm in enumerate(opt["ShotNames"].split(",")):
            if i:
                time.sleep(float(opt.get("ShotDelay", 0.7)))
            ewpng.write_png(os.path.join(opt["Shots"], f"{nm}_{w}x{h}.png"), w, h, bytearray(pixel * (w * h)))
            say(f"SELFTEST named shot {nm} saved")
    for i in range(int(opt.get("ShotCount", 1)) if not opt.get("ShotNames") else 0):
        if i:
            time.sleep(0.7)
        name = f"shot_{w}x{h}.png" if i == 0 else f"shot_{w}x{h}_{i}.png"
        ewpng.write_png(os.path.join(opt["Shots"], name), w, h, bytearray(pixel * (w * h)))
        say(f"SELFTEST shot {w}x{h} saved")
if mode == "fail":
    say("SELFTEST Logic FAILED: expected 3 got 2")
elif mode == "crash":
    say("LogWindows: Error: === Critical error ===")
    say("LogWindows: Error: Fatal error: [File:Fake.cpp] [Line: 1]")
    log.close()
    sys.exit(3)
elif mode == "hang":
    time.sleep(120)
else:
    say("SELFTEST Logic PASSED (12/12)")
if mode == "shothang":
    # Like a HighResShot run: the screenshots are written, then the game keeps running and never says it is done.
    time.sleep(120)
if mode == "passhang":
    say("SELFTEST DONE")
    time.sleep(120)
if mode == "slowdone":
    # Every line comes after a pause, so each read of the log ends with a line break: done must still be seen.
    time.sleep(1.5)
    say("SELFTEST DONE")
    time.sleep(120)
log.close()
sys.exit(int(opt.get("Code", 0)))
