"""A stand-in for a game, used by test_ewtool.py (no Unreal needed).

Called as: python ewtool_fakegame.py <url> [-ResX=W -ResY=H ...] -abslog=<file> ...
The URL options choose what it does: Mode=pass|fail|hang|crash|passhang|nolog|quiet, Shots=<dir> (writes one PNG
of the requested size there), Mangle=1 (logs a command line with the URL cut at the first '=', like a broken
batch file), Code=<n> (exit code).
"""
import os, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Plugins", "Enginewright", "Scripts"))
import ew_png  # noqa: E402

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
if opt.get("Shots"):
    w, h = int(val("ResX") or 64), int(val("ResY") or 32)
    os.makedirs(opt["Shots"], exist_ok=True)
    ew_png.write_png(os.path.join(opt["Shots"], f"shot_{w}x{h}.png"), w, h, bytearray(b"\x10\x80\xf0\xff" * (w * h)))
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
if mode == "passhang":
    say("SELFTEST DONE")
    time.sleep(120)
log.close()
sys.exit(int(opt.get("Code", 0)))
