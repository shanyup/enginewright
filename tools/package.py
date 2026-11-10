"""Builds the Enginewright plugin for one engine version and packs it as a release zip.

Usage:
    python tools/package.py --engine "C:/Program Files/Epic Games/UE_5.8"
    python tools/package.py --engine <engine root> --out dist --platform Win64
    python tools/package.py --engine <engine root> --mvvm-from <project folder>
    python tools/package.py --engine <engine root> --mvvm-from <project folder> --mvvm-only

Steps: RunUAT BuildPlugin (compiles the plugin against that engine, outside any project), check the
result (uplugin + editor DLL present, no Intermediate/ or debug files), then zip it as
dist/Enginewright-<version>-UE<major.minor>-<platform>.zip with a top-level Enginewright/ folder,
ready to unzip into a project's Plugins/ folder. Prints the zip path and its SHA-256.

With --mvvm-from, also packs the optional EnginewrightMVVM plugin from that project's build. RunUAT
BuildPlugin cannot build it alone (it depends on Enginewright), so its binaries come from a project
that builds both plugins. The project copy of the MVVM source must match the repo and its DLL must be
newer, and the engine BuildId must match. The result is
dist/EnginewrightMVVM-<version>-UE<major.minor>-<platform>.zip with a top-level EnginewrightMVVM/
folder (Source from the repo, Binaries from the project). --mvvm-only skips BuildPlugin and the main
zip, so the MVVM zip can be redone alone.
Python 3 standard library only.
"""
import argparse, hashlib, json, os, pathlib, shutil, subprocess, sys, time, zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "Plugins" / "Enginewright" / "Enginewright.uplugin"
MVVM = ROOT / "Plugins" / "EnginewrightMVVM"
SKIP_DIRS = {"Intermediate", "Saved"}
SKIP_SUFFIXES = {".pdb", ".exp", ".lib", ".obj", ".ilk"}
MVVM_BANNED_SUFFIXES = {".pdb", ".lib", ".exp"}


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


def pack_mvvm(engine: pathlib.Path, ue: str, platform: str, out: pathlib.Path, project: pathlib.Path) -> None:
    uplugin = MVVM / "EnginewrightMVVM.uplugin"
    if not uplugin.is_file():
        sys.exit(f"MVVM plugin not found in the repo: {uplugin}")
    version = json.loads(uplugin.read_text(encoding="utf-8"))["VersionName"]

    proj_plugin = project / "Plugins" / "EnginewrightMVVM"
    proj_uplugin = proj_plugin / "EnginewrightMVVM.uplugin"
    if not proj_uplugin.is_file():
        sys.exit(f"Project has no EnginewrightMVVM plugin: {proj_uplugin}")
    proj_meta = json.loads(proj_uplugin.read_text(encoding="utf-8"))

    binaries = proj_plugin / "Binaries" / platform
    dll = binaries / "UnrealEditor-EnginewrightMVVM.dll"
    modules = binaries / "UnrealEditor.modules"
    if not dll.is_file():
        sys.exit(f"Editor module missing in the project build: {dll}")
    if not modules.is_file():
        sys.exit(f"Module manifest missing in the project build: {modules}")

    # Project-built binaries carry the engine BuildId, same as BuildPlugin output.
    eng_modules = engine / "Engine" / "Binaries" / platform / "UnrealEditor.modules"
    if not eng_modules.is_file():
        sys.exit(f"Engine module manifest not found: {eng_modules}")
    eng_id = json.loads(eng_modules.read_text(encoding="utf-8")).get("BuildId")
    proj_id = json.loads(modules.read_text(encoding="utf-8")).get("BuildId")
    if not eng_id or not proj_id or str(eng_id) != str(proj_id):
        sys.exit(f"BuildId mismatch: project plugin was built with {proj_id}, engine is {eng_id}. "
                 "Rebuild the project with this engine.")

    # The project source must be exactly the repo source (line endings normalized).
    repo_src = MVVM / "Source"
    proj_src = proj_plugin / "Source"

    def normalized(p: pathlib.Path) -> bytes:
        return p.read_bytes().replace(b"\r\n", b"\n")

    repo_files = {p.relative_to(repo_src).as_posix(): normalized(p)
                  for p in repo_src.rglob("*") if p.is_file()}
    proj_files = {p.relative_to(proj_src).as_posix(): normalized(p)
                  for p in proj_src.rglob("*") if p.is_file()} if proj_src.is_dir() else {}
    diff = sorted(k for k in set(repo_files) | set(proj_files) if repo_files.get(k) != proj_files.get(k))
    if diff:
        more = " ..." if len(diff) > 10 else ""
        sys.exit("Project copy of the MVVM source differs from the repo: "
                 + ", ".join(diff[:10]) + more + "\nCopy the repo plugin into the project and rebuild.")

    # The DLL must be newer than every repo source file of the plugin.
    dll_mtime = dll.stat().st_mtime
    stale = [p.relative_to(repo_src).as_posix() for p in repo_src.rglob("*")
             if p.is_file() and p.stat().st_mtime >= dll_mtime]
    if stale:
        sys.exit("The project DLL is older than " + ", ".join(stale[:5]) + ". Rebuild the project first.")

    # All three VersionName values must agree with the Enginewright plugin.
    ew_version = json.loads(PLUGIN.read_text(encoding="utf-8"))["VersionName"]
    if version != ew_version or proj_meta.get("VersionName") != ew_version:
        sys.exit(f"VersionName mismatch: repo MVVM {version}, project MVVM "
                 f"{proj_meta.get('VersionName')}, Enginewright {ew_version}.")

    out.mkdir(parents=True, exist_ok=True)
    zpath = out / f"EnginewrightMVVM-{version}-UE{ue}-{platform}.zip"
    if zpath.exists():
        zpath.unlink()
    n = 0
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(uplugin, "EnginewrightMVVM/EnginewrightMVVM.uplugin")
        n += 1
        for p in sorted(repo_src.rglob("*")):
            if p.is_file():
                z.write(p, pathlib.PurePosixPath("EnginewrightMVVM", "Source", *p.relative_to(repo_src).parts))
                n += 1
        for sub in ("Resources", "Config"):
            d = MVVM / sub
            if d.is_dir():
                for p in sorted(d.rglob("*")):
                    if p.is_file():
                        z.write(p, pathlib.PurePosixPath("EnginewrightMVVM", sub, *p.relative_to(d).parts))
                        n += 1
        for f in (dll, modules):
            z.write(f, pathlib.PurePosixPath("EnginewrightMVVM", "Binaries", platform, f.name))
            n += 1
        if (ROOT / "LICENSE").is_file():
            z.write(ROOT / "LICENSE", "EnginewrightMVVM/LICENSE")
            n += 1
    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        bad = [x for x in names if "/Intermediate/" in x
               or pathlib.PurePosixPath(x).suffix.lower() in MVVM_BANNED_SUFFIXES]
        if bad:
            sys.exit(f"MVVM zip contains files that should not ship: {bad[:5]}")
        missing = [x for x in ("EnginewrightMVVM/EnginewrightMVVM.uplugin",
                               f"EnginewrightMVVM/Binaries/{platform}/UnrealEditor-EnginewrightMVVM.dll")
                   if x not in names]
        if missing:
            sys.exit(f"MVVM zip is missing {missing}")
    sha = hashlib.sha256(zpath.read_bytes()).hexdigest()
    zpath.with_name(zpath.name + ".sha256").write_bytes(f"{sha}  {zpath.name}\n".encode())
    print(f"OK {zpath} ({n} files, {zpath.stat().st_size // 1024} KB)\nSHA-256 {sha}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", required=True, help="Engine root folder, e.g. C:/Program Files/Epic Games/UE_5.8")
    ap.add_argument("--out", default=str(ROOT / "dist"), help="Output folder (default dist/)")
    ap.add_argument("--platform", default="Win64", help="Target platform (default Win64)")
    ap.add_argument("--keep-build", action="store_true", help="Keep the unzipped build folder")
    ap.add_argument("--mvvm-from", metavar="PROJECT", help="Project folder with a built EnginewrightMVVM (adds the MVVM zip)")
    ap.add_argument("--mvvm-only", action="store_true", help="Only pack the MVVM zip (needs --mvvm-from)")
    a = ap.parse_args()

    if a.mvvm_only and not a.mvvm_from:
        sys.exit("--mvvm-only needs --mvvm-from <project folder>")
    engine = pathlib.Path(a.engine)
    ue = engine_version(engine)
    out = pathlib.Path(a.out)
    if a.mvvm_only:
        pack_mvvm(engine, ue, a.platform, out, pathlib.Path(a.mvvm_from))
        return

    meta = json.loads(PLUGIN.read_text(encoding="utf-8"))
    version = meta["VersionName"]
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
        missing = [f"Enginewright/Scripts/{s}" for s in ("ewtool.py",) if f"Enginewright/Scripts/{s}" not in names]
        if missing:
            sys.exit(f"Zip is missing {missing} (Config/FilterPlugin.ini lists /Scripts/*.py).")
    if not a.keep_build:
        shutil.rmtree(build.parent)
    sha = hashlib.sha256(zpath.read_bytes()).hexdigest()
    # Same format as sha256sum, so `sha256sum -c` (or Get-FileHash) can check the download.
    zpath.with_name(zpath.name + ".sha256").write_bytes(f"{sha}  {zpath.name}\n".encode())  # LF only: sha256sum -c rejects CRLF
    print(f"OK {zpath} ({n} files, {zpath.stat().st_size // 1024} KB)\nSHA-256 {sha}")
    if a.mvvm_from:
        pack_mvvm(engine, ue, a.platform, out, pathlib.Path(a.mvvm_from))


if __name__ == "__main__":
    main()
