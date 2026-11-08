"""Records the README demo: drives a running editor through Enginewright and captures the editor window.

Maintainers only (not part of the plugin). Windows, an open editor with the plugin, Python 3.
Step 1 (this script, `record`): runs the agent steps over MCP and saves after each step a capture of the editor
window (PrintWindow, works while the window is covered) plus the reply, into <out>/steps.json + PNGs.
Step 2 (`render`, needs Pillow): builds docs/media/demo.gif and the still images from that folder.

    python tools/make_demo.py record --project D:/P/Demo --out build/demo
    python tools/make_demo.py render --out build/demo
"""
import argparse, json, os, pathlib, subprocess, sys, time, urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
PS = r'''
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class PW {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$h = (Get-Process UnrealEditor | Where-Object { $_.MainWindowTitle -like "*TITLE*" } | Select-Object -First 1).MainWindowHandle
$r = New-Object PW+RECT; [PW]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
[PW]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save("OUT", [System.Drawing.Imaging.ImageFormat]::Png)
'''


class Client:
    def __init__(self, project, url="http://127.0.0.1:3100/mcp"):
        self.url, self.n = url, 0
        self.tok = (pathlib.Path(project) / "Saved/Enginewright/token").read_text().strip()

    def call(self, name, args=None, timeout=300):
        self.n += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self.n, "method": "tools/call",
                           "params": {"name": "call", "arguments": {"name": name, "args": args or {}}}}).encode()
        req = urllib.request.Request(self.url, data=body, method="POST", headers={
            "Content-Type": "application/json", "Accept": "application/json", "X-Enginewright-Token": self.tok})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())["result"]["structuredContent"]


def capture(title, out):
    script = PS.replace("TITLE", title).replace("OUT", str(out))
    subprocess.run(["powershell", "-NoProfile", "-Command", script], check=True, capture_output=True)


def record(a):
    out = pathlib.Path(a.out); out.mkdir(parents=True, exist_ok=True)
    c, title = Client(a.project), pathlib.Path(a.project).name
    steps = []
    W = "/Game/UI/WBP_MainMenu"

    def step(label, name, args, show=None, note=None, settle=0.5, after=None):
        r = c.call(name, args)
        if after:
            after()
        if name == "widget_add" or show == "render":
            # The agent checks its work: render the widget after each change.
            rr = c.call("widget_render", {"path": W, "width": 1920, "height": 1080, "inline": False})
            show = (rr.get("data") or {}).get("file")
        time.sleep(settle)
        png = out / f"step{len(steps):02d}.png"
        capture(title, png)
        steps.append({"label": label, "tool": name, "args": args, "reply": r, "capture": png.name, "show": show, "note": note})
        print(f"{name}: ok={r.get('ok')} {json.dumps(r)[:160]}", flush=True)
        return r

    # Clean start: no leftovers from a previous recording.
    if c.call("pie_state").get("data", {}).get("playing"):
        c.call("pie_stop")
    if c.call("asset_exists", {"path": W}).get("data", {}).get("exists"):
        r = c.call("asset_delete", {"paths": [W]})
        if not r.get("ok"):
            sys.exit(f"could not delete the old {W}: {r}")
    step("Check the editor", "doctor", {})
    step("Create a menu widget", "widget_create", {"path": W})
    step("Add a title", "widget_add", {"path": W, "class": "TextBlock", "name": "Title",
         "properties": {"Text": "MY GAME", "Font": {"Size": 72}, "Justification": "Center"},
         "slot": {"position": [0, 220], "size": [1920, 110], "anchors": [0, 0, 0, 0]}})
    for i, (name, label) in enumerate((("PlayButton", "Play"), ("SettingsButton", "Settings"), ("QuitButton", "Quit"))):
        step(f"Add the {label} button", "widget_add", {"path": W, "class": "Button", "name": name,
             "slot": {"position": [760, 420 + i * 130], "size": [400, 100]}},
             after=lambda name=name, label=label: c.call("widget_add", {"path": W, "class": "TextBlock",
                 "name": name.replace("Button", "Label"), "parent": name,
                 "properties": {"Text": label, "Font": {"Size": 36}, "ColorAndOpacity": {"SpecifiedColor": {"R": 0.05, "G": 0.05, "B": 0.05, "A": 1}}}}))
    step("Cover Settings with a 'coming soon' banner", "widget_add", {"path": W, "class": "Image", "name": "SoonBanner",
         "properties": {"ColorAndOpacity": {"R": 1, "G": 0.55, "B": 0.1, "A": 0.85}},
         "slot": {"position": [700, 560], "size": [520, 60]}})
    step("Start Play In Editor", "pie_start", {"timeout": 120}, settle=3)
    step("Open the menu on the player's screen", "ui_open", {"class": W}, settle=1)
    step("Read the screen", "ui_state", {})
    step("Click Play", "ui_click", {"button": "Play"}, settle=1)
    step("Click Settings (covered)", "ui_click", {"button": "Settings"})
    r = step("Screenshot what the player sees", "screenshot", {"inline": False, "maxWidth": 1280})
    shot = (r.get("data") or {}).get("file")
    if shot:
        steps[-1]["show"] = shot
    step("Stop Play In Editor", "pie_stop", {}, settle=1)
    # The game itself, outside the editor, at two resolutions; then both screenshots side by side.
    r = c.call("game_run", {"url": "/Engine/Maps/Templates/Template_Default?Demo=1", "resolutions": ["1280x720", "1024x768"],
                            "args": ["-ExecCmds=HighResShot 1"], "done": "Screenshot", "doneGrace": 5, "filter": ["Screenshot"],
                            "timeout": 600, "wait": 900}, timeout=1000)
    (out / "game_run.json").write_text(json.dumps(r, indent=1), encoding="utf-8")
    shots = [((run.get("screenshots") or [{}])[0]).get("path") for run in (r.get("data") or {}).get("runs", [])]
    print("game_run:", r.get("ok"), shots, flush=True)
    if len(shots) == 2 and all(shots):
        r2 = c.call("image_compose", {"title": "game_run: Template_Default at two resolutions", "columns": ["1280x720", "1024x768"],
                                      "images": shots, "cellWidth": 800, "out": str(out / "compose.png")})
        print("image_compose:", json.dumps(r2)[:300], flush=True)
    (out / "steps.json").write_text(json.dumps(steps, indent=1), encoding="utf-8")
    print("recorded", len(steps), "steps in", out)


# ------------------------------------------------------------------ render
def summary(s):
    """One short reply line per step, from the real reply."""
    r, d = s["reply"], s["reply"].get("data") or {}
    if not r.get("ok"):
        return f"× {r.get('errorCode')}: {r.get('message', '')}"
    t = s["tool"]
    if t == "doctor":
        return f"✓ healthy={d.get('healthy')}  engine {d.get('engine', '').split('-')[0]}"
    if t == "widget_create":
        return f"✓ created {d.get('path', '').split('.')[0]}"
    if t == "widget_add":
        w = d.get("widget") or d
        return f"✓ added {w.get('name', s['args']['name'])} ({s['args']['class']}), compiled, saved, read back"
    if t == "widget_render":
        return f"✓ {d.get('width')}x{d.get('height')} PNG → {pathlib.Path(d.get('file', '')).name}"
    if t == "pie_start":
        return "✓ playing, every player has a pawn"
    if t == "ui_open":
        return f"✓ in viewport: {d.get('inViewport')}"
    if t == "ui_state":
        return "✓ buttons: " + ", ".join(b.get("label", b.get("name")) for b in d.get("buttons", []))
    if t == "ui_click":
        cl = d.get("clicked") or {}
        return f"✓ clicked {cl.get('name')} (method {cl.get('method')}, nothing drawn over it)"
    if t == "screenshot":
        return f"✓ {pathlib.Path(d.get('file', '')).name}, brightness {d.get('meanBrightness', d.get('brightness', '?'))}"
    if t == "pie_stop":
        return "✓ stopped"
    return "✓ ok"


def args_text(s):
    a = dict(s["args"])
    for k in ("path", "class"):
        if isinstance(a.get(k), str) and a[k].startswith("/Game/"):
            a[k] = a[k]
    t = json.dumps(a, ensure_ascii=False)
    return t if len(t) < 70 else t[:67] + "…}"


def render(a):
    from PIL import Image, ImageDraw, ImageFont
    out = pathlib.Path(a.out)
    steps = json.loads((out / "steps.json").read_text(encoding="utf-8"))
    media = ROOT / "docs" / "media"; media.mkdir(parents=True, exist_ok=True)
    W, EH, PH = 1200, 650, 230
    mono = ImageFont.truetype("C:/Windows/Fonts/CascadiaMono.ttf", 19)
    mono_b = ImageFont.truetype("C:/Windows/Fonts/consolab.ttf", 20)
    head = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 22)
    small = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 18)
    log, frames, durations, stills = [], [], [], {}
    for i, s in enumerate(steps):
        ed = Image.open(out / s["capture"]).convert("RGB")
        ed = ed.crop((8, 0, ed.width - 8, ed.height - 8))   # window border
        ed = ed.resize((W, int(ed.height * W / ed.width)), Image.LANCZOS).crop((0, 0, W, EH))
        img = Image.new("RGB", (W, EH + PH), (18, 18, 22))
        img.paste(ed, (0, 0))
        if s.get("show") and os.path.isfile(s["show"]):
            ins = Image.open(s["show"]).convert("RGB")
            iw = 520; ins = ins.resize((iw, int(ins.height * iw / ins.width)), Image.LANCZOS)
            x, y = W - iw - 24, 70
            d = ImageDraw.Draw(img)
            d.rectangle((x - 4, y - 34, x + iw + 4, y + ins.height + 4), fill=(255, 196, 0))
            cap = "widget_render: the agent checks its work" if s["tool"] == "widget_add" else f"{s['tool']}: what the agent sees"
            d.text((x + 6, y - 31), cap, font=small, fill=(0, 0, 0))
            img.paste(ins, (x, y))
        d = ImageDraw.Draw(img)
        d.rectangle((0, EH, W, EH + 38), fill=(36, 36, 44))
        d.text((16, EH + 6), f"Agent: {s['label']}", font=head, fill=(255, 255, 255))
        d.text((W - 300, EH + 9), f"step {i + 1}/{len(steps)}  ·  Enginewright", font=small, fill=(150, 150, 165))
        log.append((f"› {s['tool']} {args_text(s)}", summary(s), s["reply"].get("ok")))
        y = EH + 50
        for cmd, res, ok in log[-3:]:
            res = res if len(res) <= 92 else res[:91] + "…"
            d.text((16, y), cmd, font=mono_b, fill=(120, 200, 255)); y += 28
            d.text((40, y), res, font=mono, fill=(130, 230, 130) if ok else (255, 130, 110)); y += 32
        frames.append(img)
        durations.append(2600 if s["tool"] in ("ui_click", "widget_render", "screenshot", "doctor") else 1700)
        stills[s["tool"] + str(i)] = img
    durations[-1] = 3500
    gif = media / "demo.gif"
    pal = [f.quantize(colors=200, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for f in frames]
    pal[0].save(gif, save_all=True, append_images=pal[1:], duration=durations, loop=0, optimize=True)
    print("gif", gif, gif.stat().st_size // 1024, "KB", len(frames), "frames")
    for i, f in enumerate(frames):
        f.save(out / f"frame{i:02d}.png")
    # Still images for the README.
    tools = [s["tool"] for s in steps]
    frames[tools.index("doctor")].save(media / "install-doctor.png", optimize=True)
    covered = [i for i, s in enumerate(steps) if s["tool"] == "ui_click" and not s["reply"].get("ok")]
    if covered:
        frames[covered[0]].save(media / "ui-click-covered.png", optimize=True)
    last_add = [s for s in steps if s["tool"] == "widget_add" and s.get("show")]
    if last_add:
        Image.open(last_add[-1]["show"]).convert("RGB").resize((1280, 720), Image.LANCZOS).save(media / "widget-render.png", optimize=True)
    if (out / "compose.png").is_file():
        Image.open(out / "compose.png").convert("RGB").save(media / "game-run-compose.png", optimize=True)
    for f in sorted(media.iterdir()):
        print(f.name, f.stat().st_size // 1024, "KB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["record", "render"])
    ap.add_argument("--project")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    record(a) if a.cmd == "record" else render(a)


if __name__ == "__main__":
    main()
