"""Builds the Enginewright plugin for one engine version and packs it as a release zip.

Usage:
    python tools/package.py --engine "C:/Program Files/Epic Games/UE_5.8"
    python tools/package.py --engine <engine root> --out dist --platform Win64

Steps: RunUAT BuildPlugin (compiles the plugin against that engine, outside any project), check the
result (uplugin + editor DLL present, no Intermediate/ or debug files), then zip it as
dist/Enginewright-<version>-UE<major.minor>-<platform>.zip with a top-level Enginewright/ folder,
ready to unzip into a project's Plugins/ folder. Prints the zip path and its SHA-256.
Python 3 standard library only.
"""
import argparse, hashlib, json, os, pathlib, shutil, subprocess, sys, time, zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "Plugins" / "Enginewright" / "Enginewright.uplugin"
SKIP_DIRS = {"Intermediate", "Saved"}
SKIP_SUFFIXES = {".pdb", ".exp", ".lib", ".obj", ".ilk"}


def engine_version(engine: pathlib.Path) -> str:
    f = engine / "Engine" / "Build" / "Build.version"
    if not f.is_file():
        sys.exit(f"Not an engine folder (no Engine/Build/Build.version): {engine}")
    v = json.loads(f.read_text(encoding="utf-8"))
    return f"{v['MajorVersion']}.{v['MinorVersion']}"


def run_uat(engine: pathlib.Path) -> pathlib.Path:
    bat = engine / "Engine" / "Build" / "BatchFiles" / ("RunUAT.bat" if os.name == "nt" else "RunUAT.sh")
    if not bat.is_file():
        sys.exit(f"RunUAT not found: {bat}")
    return bat


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", required=True, help="Engine root folder, e.g. C:/Program Files/Epic Games/UE_5.8")
    ap.add_argument("--out", default=str(ROOT / "dist"), help="Output folder (default dist/)")
    ap.add_argument("--platform", default="Win64", help="Target platform (default Win64)")
    ap.add_argument("--keep-build", action="store_true", help="Keep the unzipped build folder")
    a = ap.parse_args()

    engine = pathlib.Path(a.engine)
    ue = engine_version(engine)
    meta = json.loads(PLUGIN.read_text(encoding="utf-8"))
    version = meta["VersionName"]
    out = pathlib.Path(a.out)
    build = out / f"build-UE{ue}-{a.platform}" / "Enginewright"
    if build.parent.exists():
        shutil.rmtree(build.parent)
    build.parent.mkdir(parents=True)

    cmd = [str(run_uat(engine)), "BuildPlugin", f"-Plugin={PLUGIN}", f"-Package={build}",
           f"-TargetPlatforms={a.platform}", "-Rocket"]
    print("Running:", " ".join(cmd), flush=True)
    log = out / f"build-UE{ue}-{a.platform}.log"
    for attempt in range(30):
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            code = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, shell=(os.name == "nt"))
        # BuildPlugin does not wait for another UnrealBuildTool (one build at a time per engine): wait and retry.
        if code != 0 and "ConflictingInstance" in log.read_text(encoding="utf-8", errors="replace"):
            print("Another Unreal build is running; waiting 60 s...", flush=True)
            time.sleep(60)
            continue
        break
    if code != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-25:]
        sys.exit(f"BuildPlugin failed (exit {code}). Log: {log}\n" + "\n".join(tail))

    # Check the result before packing it.
    dll = build / "Binaries" / a.platform / "UnrealEditor-Enginewright.dll"
    if a.platform == "Win64" and not dll.is_file():
        sys.exit(f"Build finished but the editor module is missing: {dll}")
    if not (build / "Enginewright.uplugin").is_file():
        sys.exit("Build finished but Enginewright.uplugin is missing from the package.")
    packaged = json.loads((build / "Enginewright.uplugin").read_text(encoding="utf-8"))
    if packaged.get("EngineVersion", "").rsplit(".", 1)[0] not in (ue, f"{ue}.0", ""):
        print(f"Note: packaged EngineVersion is {packaged.get('EngineVersion')}", flush=True)

    zpath = out / f"Enginewright-{version}-UE{ue}-{a.platform}.zip"
    if zpath.exists():
        zpath.unlink()
    n = 0
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(build.rglob("*")):
            rel = p.relative_to(build)
            if p.is_dir() or rel.parts[0] in SKIP_DIRS or p.suffix.lower() in SKIP_SUFFIXES:
                continue
            z.write(p, pathlib.PurePosixPath("Enginewright", *rel.parts))
            n += 1
    for extra in ("LICENSE", "README.md"):
        if (ROOT / extra).is_file():
            with zipfile.ZipFile(zpath, "a", zipfile.ZIP_DEFLATED) as z:
                z.write(ROOT / extra, f"Enginewright/{extra}")
                n += 1
    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        bad = [x for x in names if "/Intermediate/" in x or x.endswith(".pdb")]
        if bad:
            sys.exit(f"Zip contains files that should not ship: {bad[:5]}")
        if a.platform == "Win64" and "Enginewright/Binaries/Win64/UnrealEditor-Enginewright.dll" not in names:
            sys.exit("Zip is missing the editor DLL.")
        missing = [f"Enginewright/Scripts/{s}" for s in ("ewtool.py", "ew_png.py", "ew_font.py") if f"Enginewright/Scripts/{s}" not in names]
        if missing:
            sys.exit(f"Zip is missing {missing} (Config/FilterPlugin.ini lists /Scripts/*.py).")
    if not a.keep_build:
        shutil.rmtree(build.parent)
    sha = hashlib.sha256(zpath.read_bytes()).hexdigest()
    print(f"OK {zpath} ({n} files, {zpath.stat().st_size // 1024} KB)\nSHA-256 {sha}")


if __name__ == "__main__":
    main()
