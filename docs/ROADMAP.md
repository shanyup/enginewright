# Roadmap

This is the current plan, not a promise. There are no dates. Order and scope can change.

## v0.1 (done)

In-editor HTTP MCP server with token, three meta tools (`search`, `describe`, `call`), strict validation with read-back, object, asset, DataTable, level, editor, project settings and log tools, `doctor`, and atomic `batch` with rollback.

## v0.2 (current)

- Prebuilt UE 5.8 Win64 zip (`tools/package.py`) and ready-to-paste client configs for Claude Code, Codex, Cursor, VS Code and Hermes (`client_config`).
- Game camera screenshot and before/after screenshot (`screenshot`, `screenshot_ab`).
- Play In Editor control: 1-4 players, standalone / listen / client net mode, wait for pawns, synthetic input (`pie_input`), wait for a log line (`log_wait`).
- Full C++ build loop (`editor_restart` with build, `build_status`), Live Coding, automation tests (`automation_list`, `automation_run`).
- Blueprint, Material and Material Instance, mesh and texture tools; enum and struct creation.
- Import while the editor is open (texture, sound, font, mesh), Enhanced Input helpers, validation (Blueprints, data validation, broken references, map check).
- Content checks: `id_audit` (IDs used but never defined), `key_audit` (hard-coded input keys).
- Test suites that build their own fixtures and run on a blank project (`tests/run_all.py`).

Tested on UE 5.8 Win64 only.

## v0.3 (planned next)

- Safer rename: also report text and ID references to the old name (DataTable rows, string fields) that a redirector cannot fix.
- Measuring helpers: size of a Blueprint or class (temporary spawn, bounds, automatic clean-up), trace grids.
- Multiplayer tests on one machine: network emulation profiles and replication checks ("does client 2 see X").
- UMG widgets and Gameplay Tags.

## Later (not started)

- Behavior Tree / StateTree editing; AnimBP state machines.
- Niagara, Sequencer and other larger editor areas.
- A headless mode (same tools with the editor closed), packaging, localization.
- Testing and prebuilt zips for older engine versions (5.4 to 5.7) and other platforms.
