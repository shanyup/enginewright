# Roadmap

This is the current plan, not a promise. There are no dates. Order and scope can change.

## v0.1 (current)

Done: in-editor HTTP MCP server with token, three meta tools (`search`, `describe`, `call`), strict validation with read-back, object, asset, DataTable, level, editor, project settings and log tools, `doctor`, and atomic `batch` with rollback. Tested on UE 5.8 Win64 only.

Still open for the v0.1 release: a prebuilt UE 5.8 Win64 plugin zip on GitHub Releases.

## v0.2 (planned next)

- Game camera screenshot (what the player sees, not the editor viewport).
- Play In Editor control: player count, net mode, simulated input.
- Full C++ build: save, close the editor, build, reopen and reconnect.
- Blueprint, Material and Material Instance, mesh and texture tools; Live Coding; running automation tests.
- Import while the editor is open (texture, sound, font, mesh), Enhanced Input helpers, validation (compile all Blueprints, data validation, broken references).

## Later (not started)

- UMG, Gameplay Tags; Niagara, Sequencer and other larger editor areas.
- Behavior Tree / StateTree editing.
- A headless mode (same tools with the editor closed), packaging, localization.
- Testing and prebuilt zips for older engine versions (5.4 to 5.7) and other platforms.
