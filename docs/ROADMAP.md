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

## v0.3 (current)

- Done: safer rename (`asset_rename` reports text and ID references to the old name that a redirector cannot fix; `dryRun` preview).
- Done: measuring helpers: `class_measure` (temporary spawn, bounds, automatic clean-up), `screen_coverage` (size on a player's screen).
- Done: multiplayer tests on one machine: `pie_actors` (replication checks per world, waits for replication), `pie_net_emulation` (network emulation profiles).
- Done: UMG widgets (create, tree, add / set / remove with slot layout, render to PNG) and Gameplay Tags (list, add, remove).
- Done: runtime UI with CommonUI support: `ui_state` (activatable widgets and stacks, buttons), `ui_click`, `ui_open`, `ui_close`; CommonUI assets (activatable pages, CommonButtonBase, text / button styles) are built with the Blueprint and widget tools.
- Done: CommonUI input: `ui_input_audit` (input action wiring checks), `ui_open` with `stack` (push onto a running activatable stack, `ui_close` pops it), `ui_input_type` (switch input type); `object_set` on class defaults also updates placed instances and widget templates that still had the old default.
- Done: localization: `stringtable_list`, `stringtable_get`, `stringtable_set` (String Table assets, read back), `stringtable_compare` (missing, extra, empty, untranslated, format argument / rich text tag differences), `loc_culture` (switch the editor language).
- Next: MVVM.

## Later (not started)

- Behavior Tree / StateTree editing; AnimBP state machines.
- Niagara, Sequencer and other larger editor areas.
- A headless mode (same tools with the editor closed), packaging, localization.
- Testing and prebuilt zips for older engine versions (5.4 to 5.7) and other platforms.
