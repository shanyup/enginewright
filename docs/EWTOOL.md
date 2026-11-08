# ewtool.py — compile the project while the editor is closed (optional)

Enginewright needs no Python: the MCP server and every tool run inside the editor. `ewtool.py` is an optional
helper for the one job the editor cannot do for itself: compiling the editor target, which is only possible while
the editor is closed (it locks its own DLLs). Running the game, self-tests, screenshots at several resolutions and
side-by-side comparison images are MCP tools (`game_run`, `game_run_status`, `image_compose`; see the README).

`Plugins/Enginewright/Scripts/ewtool.py` — Python 3, standard library only. Every command prints one JSON object.

| Exit code | Meaning |
|---|---|
| 0 | ok (build succeeded) |
| 1 | finished, but not ok (build failed, or the build was lost) |
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
- UnrealBuildTool runs with `-WaitMutex` in a detached worker. On Windows the worker is started through WMI, so it
  is outside the caller's process tree and job object: when `--wait` runs out, or when the caller is killed (for
  example an agent's tool time limit ends the shell), UBT keeps going. A build is never killed half way (that forces
  a full rebuild). `--background` returns at once. While waiting for another build, `otherBuilds` names it.
- The result lists `errors` (compiler/linker lines), `actions` (`[n/m]`), `upToDate`, and for a failed link the
  locked file with its holder.
- `--ubt-arg` passes extra UBT arguments (e.g. `--ubt-arg=-ForceUnity`).

`status --project … [--id <id>|latest] [--wait N]` follows a build (`running` / `succeeded` / `failed` / `lost`).
`locks --project …` lists holders, open editors of the project and running builds.

While the editor is open, use the MCP tools instead: `live_coding_compile`, or `editor_restart` with `build: true`
(saves, closes, builds and reopens the editor).

## Tests

`python tests/run_all.py ewtool` (no editor needed; the build checks use UBT on the test project, or check the
`LOCKED` answer when its editor is open). The ER-05 checks kill the caller of `build --wait` the way agent tools do
(`taskkill /T` on its process tree, and a kill-on-close job object) and verify that the build still finishes.
