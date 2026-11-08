# ewtool.py — build, run and check a project from outside the editor

The MCP server runs inside the editor. Some jobs cannot: compiling the editor target (the open editor locks its
DLLs), or starting the game itself to run a self-test or take screenshots at several resolutions.
`Plugins/Enginewright/Scripts/ewtool.py` does these from the command line. Python 3, standard library only
(Pillow is used for faster image decoding when it is installed). Every command prints one JSON object.

| Exit code | Meaning |
|---|---|
| 0 | ok (build succeeded, every run PASSED / EXITED_OK, image written) |
| 1 | finished, but not ok (build failed, a run FAILED / TIMEOUT / CRASHED / …) |
| 2 | refused before starting (`error` code: `LOCKED`, `BAD_ARGS`, `NOT_FOUND`, `NO_ENGINE`, …) |
| 3 | build still running (it keeps running; follow it with `status`) |

The engine is found from the project's `EngineAssociation` (registry / Epic launcher), or `--engine` / `UE_ENGINE_DIR`.
Output goes to `<Project>/Saved/Enginewright/runs/`.

## build

```bash
python Plugins/Enginewright/Scripts/ewtool.py build --project MyGame.uproject [--target MyGameEditor] [--wait 3600]
```

- Before compiling it checks the project and plugin binaries. If a process holds one open (an open editor, a game,
  a process that will not die), it waits `--lock-wait` seconds (default 120), then refuses with `LOCKED` and lists
  each holder: pid, name, command line, start time and the files it holds.
- UnrealBuildTool runs with `-WaitMutex` in a detached worker. When `--wait` runs out, the command returns
  `status: running` (exit 3) and UBT keeps going: a build is never killed half way (that forces a full rebuild).
  `--background` returns at once. While waiting for another build, `otherBuilds` names it.
- The result lists `errors` (compiler/linker lines), `actions` (`[n/m]`), `upToDate`, and for a failed link the
  locked file with its holder.
- `--ubt-arg` passes extra UBT arguments (e.g. `--ubt-arg=-ForceUnity`).

`status --project … [--id <id>|latest] [--wait N]` follows a build. `locks --project …` lists holders, open
editors of the project and running builds.

## run

```bash
python ewtool.py run --project MyGame.uproject \
  --url "/Game/Maps/Menu?Test=UI?Shots={shots}?Quit=1" --res 1920x1080 --res 1280x720 \
  --filter "TEST " --pass "TEST .*PASSED" --fail "TEST .*FAILED" --timeout 600 [--build]
```

- Starts `UnrealEditor.exe <project> <url> -game -ResX -ResY -windowed -abslog=<run log> -unattended` once per
  `--res` (or `--exe <packaged game>` with `--exe-arg` before the URL). The URL is passed as one argument, unchanged.
- **URL check:** the game's own log line `Command Line:` must contain the URL. If it does not (cut by a wrapper
  script) or never appears, the game is stopped at once: `URL_MISMATCH` / `URL_NOT_CONFIRMED`. With
  `--require <regex> --require-within <s>` the game is also stopped when the test mode never starts (so the real
  game, with real saves, does not keep running by mistake).
- Placeholders in `--url` and `--arg`: `{shots}` (per-run folder), `{out}`, `{w}`, `{h}`, `{res}`.
- Ends on exit, on `--timeout` (stopped: `TIMEOUT`), or `--done-grace` seconds after a `--done <regex>` line
  (`stopReason: DONE_BUT_NOT_EXITED`: the test finished but the game did not quit — reported, not hidden).
- Verdict per run: `FAILED` (a `--fail` line), `CRASHED` (fatal error / assertion in the log), `PASSED` (a `--pass`
  line), `NO_VERDICT`, `TIMEOUT`; without patterns `EXITED_OK` / `EXIT_CODE`.
- `lines` keeps only log lines that contain a `--filter` text or match a pattern; `failLines`, `crashLines`.
- `screenshots`: new PNG files from `{shots}`, `--shots <dir>` and `Saved/Screenshots`, copied into the run folder,
  each with its real width/height and `matchesRes`.
- `--dry-run` prints the commands only.

## test

Same options as `run`, but builds first (`--no-build` to skip) and needs `--pass` and/or `--fail`:

```bash
python ewtool.py test --project MyGame.uproject --url "/Game/Maps/Test?SelfTest=Logic" \
  --filter "TEST " --filter "[Save]" --pass "TEST Logic PASSED" --fail "FAILED" --require "TEST " --timeout 420
```

If the build is still running when `--wait` ends, the game is not started (`error: BUILD_RUNNING`); follow the
build with `status` and run `test` again (the build is then up to date).

## compose

```bash
python ewtool.py compose --out cmp.png --title "Main menu" --cols "Web,Game" \
  --row 1920 web_1920.png game_1920.png --row 1280 web_1280.png game_1280.png
python ewtool.py compose --out ab.png before.png after.png
```

Puts PNG images in a labelled grid (title band, column labels, row labels; without `--cols` each cell shows its
file name). Images are scaled down to `--cell-width` (default 960, `0` = original size). The written file is read
back: size and every cell's pixels are compared with the source. Labels support Latin-1 and Latin Extended-A
(Turkish, Polish, …); `unsupportedChars` lists characters drawn as `?`.

## Tests

`python tests/run_all.py ewtool` (no editor needed; the build checks use UBT on the test project, or check the
`LOCKED` answer when its editor is open). `ENGINEWRIGHT_TEST_GAME=1` also starts the real game once.
