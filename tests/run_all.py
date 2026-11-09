"""Runs every test suite against the open editor and prints one summary line per suite.

Usage: python tests/run_all.py [suite ...]     e.g. python tests/run_all.py smoke core
Exit code 0 only when every suite passed.
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ORDER = ["smoke", "core", "blueprint", "assets", "input", "pie", "measure", "multiplayer", "umg", "tags", "ui", "audit", "loc", "settings", "game", "ewtool", "build"]


def suites():
    found = {f[5:-3] for f in os.listdir(HERE) if f.startswith("test_") and f.endswith(".py")}
    found.add("smoke")
    return [s for s in ORDER if s in found] + sorted(found - set(ORDER))


def main():
    wanted = sys.argv[1:] or [s for s in suites() if s != "build"]  # build restarts the editor: run it on purpose
    failed = 0
    for s in wanted:
        path = os.path.join(HERE, "smoke.py" if s == "smoke" else f"test_{s}.py")
        t0 = time.time()
        p = subprocess.run([sys.executable, path], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1200)
        m = re.search(r"(\d+)/(\d+) passed", p.stdout)
        ok = p.returncode == 0
        failed += not ok
        print(f"{'OK  ' if ok else 'FAIL'} {s:10s} {m.group(0) if m else 'no summary'}  ({time.time() - t0:.0f}s)")
        if not ok:
            for line in p.stdout.splitlines():
                if line.startswith("FAIL"):
                    print("     " + line[:300])
            if p.stderr.strip():
                print("     " + p.stderr.strip().splitlines()[-1][:300])
    print("ALL PASSED" if not failed else f"{failed} suite(s) failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
