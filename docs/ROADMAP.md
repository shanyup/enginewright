# Roadmap

This is the current plan, not a promise. There are no dates. Order and scope can change.

## v0.1 (done)

In-editor HTTP MCP server with token, three meta tools (`search`, `describe`, `call`), strict validation with read-back, object, asset, DataTable, level, editor, project settings and log tools, `doctor`, and atomic `batch` with rollback.

## v0.2 (done)

- Prebuilt UE 5.8 Win64 zip (`tools/package.py`) and ready-to-paste client configs for Claude Code, Codex, Cursor, VS Code and Hermes (`client_config`).
- Game camera screenshot and before/after screenshot (`screenshot`, `screenshot_ab`).
- Play In Editor control: 1-4 players, standalone / listen / client net mode, wait for pawns, synthetic input (`pie_input`), wait for a log line (`log_wait`).
- Full C++ build loop (`editor_restart` with build, `build_status`), Live Coding, automation tests (`automation_list`, `automation_run`).
- Blueprint, Material and Material Instance, mesh and texture tools; enum and struct creation.
- Import while the editor is open (texture, sound, font, mesh), Enhanced Input helpers, validation (Blueprints, data validation, broken references, map check).
- Content checks: `id_audit` (IDs used but never defined), `key_audit` (hard-coded input keys).
- Test suites that build their own fixtures and run on a blank project (`tests/run_all.py`).

Tested on UE 5.8 Win64 only.

## v0.3 (done)

- Done: safer rename (`asset_rename` reports text and ID references to the old name that a redirector cannot fix; `dryRun` preview).
- Done: measuring helpers: `class_measure` (temporary spawn, bounds, automatic clean-up), `screen_coverage` (size on a player's screen).
- Done: multiplayer tests on one machine: `pie_actors` (replication checks per world, waits for replication), `pie_net_emulation` (network emulation profiles).
- Done: UMG widgets (create, tree, add / set / remove with slot layout, render to PNG) and Gameplay Tags (list, add, remove).
- Done: runtime UI with CommonUI support: `ui_state` (activatable widgets and stacks, buttons), `ui_click`, `ui_open`, `ui_close`; CommonUI assets (activatable pages, CommonButtonBase, text / button styles) are built with the Blueprint and widget tools.
- Done: CommonUI input: `ui_input_audit` (input action wiring checks), `ui_open` with `stack` (push onto a running activatable stack, `ui_close` pops it), `ui_input_type` (switch input type); `object_set` on class defaults also updates placed instances and widget templates that still had the old default.
- Done: localization: `stringtable_list`, `stringtable_get`, `stringtable_set` (String Table assets, read back), `stringtable_compare` (missing, extra, empty, untranslated, format argument / rich text tag differences), `loc_culture` (switch the editor language).
- Done: running the game from the editor: `game_run` / `game_run_status` (the game in `-game` mode or a packaged exe, with a map URL checked against its log, one run per resolution, verdict from log patterns, filtered log lines, PNG collection), `image_compose` (labelled side-by-side PNG). No Python needed.
- Done: optional `Scripts/ewtool.py` for the one job outside the editor (see [EWTOOL.md](EWTOOL.md)): compiling while the editor is closed (waits for locks and other builds, names the process holding a DLL, UBT runs detached and outlives a killed caller).

## v0.3.1 (done, current release)

Fixes from using v0.3 on a real game project, and the contribution setup:

- `widget_render` also takes C++ UserWidget classes (UI built in code), not only Widget Blueprints.
- `game_run` refuses to start the old C++ code after a Live Coding patch (a new game process loads the DLLs on disk) and warns when C++ source changed after the last build.
- `game_run` pauses the editor's realtime level viewports while the game runs and reports a heavy editor level (Lumen, real-time sky light, volumetric clouds, many actors).
- `game_run` `shotsExpected`: screenshot runs (e.g. `HighResShot`) end when the PNG files are written, with a PASSED verdict instead of a timeout.
- Editor UI: status bar indicator (port, state, clients) and the Enginewright panel (server on/off, port, token, client configs, clients, tool categories, read-only mode, log level, recent calls, doctor); settings in Project Settings; `editor_capture`.
- Issue forms (tool request, bug report), pull request template, [CONTRIBUTING.md](../CONTRIBUTING.md), [REQUEST_WITH_AI.md](REQUEST_WITH_AI.md) and labelled good first issues.

## Version plan

A new feature raises the second number (0.x.0); a fix only raises the third (0.x.y). The 0.x releases grow with the games that use Enginewright; 1.0.0 is the launch update.

## v0.4 (next): localization

Requests from localizing a real game (English source, Turkish translation). Done on the `dev` branch, not released yet:

- Done: `stringtable_compare` reads a `.po` or `.locres` translation and a CSV source, not only two String Tables; it also reports translations made for an older source text and fuzzy entries.
- Done: `loc_gather` / `loc_gather_status`: gather, export, import, compile and check of a localization target in one call; the commandlet no longer starts an MCP server, so it works while the editor is open; the verdict uses both exit codes.
- Done: `game_run` `culture` (the game process gets the language, not only the editor).
- Done: `font_coverage` (characters of a language that a font cannot draw) and `game_run` `glyphMisses` (characters Slate could not draw in the running game).
- Open: MVVM.

## v0.5: what the RagnaCook demo still needs

- Behavior Tree / StateTree editing.
- AnimBP state machines.

## v0.6: API freeze

- Clean-up before 1.0.0: names, arguments and error codes are reviewed and fixed. No new tools.

## 1.0.0: the launch update

Built on `dev` while the 0.x releases go out; none of it reaches the public `main` before 1.0.0. If a game needs one of these earlier, the feature is built on `dev` and the game uses `dev`.

- A headless mode (the same tools with the editor closed).
- Niagara and Sequencer.
- Testing and prebuilt zips for UE 5.4 to 5.8 (and other platforms).
- FAB listing and a release-ready agent skill.
- A stability promise for the API: breaking changes only in 2.0.
- A showcase: a demo video of a game built from start to end with Enginewright, and a new README.
