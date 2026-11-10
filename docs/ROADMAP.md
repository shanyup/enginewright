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

## v0.3.1 (done)

Fixes from using v0.3 on a real game project, and the contribution setup:

- `widget_render` also takes C++ UserWidget classes (UI built in code), not only Widget Blueprints.
- `game_run` refuses to start the old C++ code after a Live Coding patch (a new game process loads the DLLs on disk) and warns when C++ source changed after the last build.
- `game_run` pauses the editor's realtime level viewports while the game runs and reports a heavy editor level (Lumen, real-time sky light, volumetric clouds, many actors).
- `game_run` `shotsExpected`: screenshot runs (e.g. `HighResShot`) end when the PNG files are written, with a PASSED verdict instead of a timeout.
- Editor UI: status bar indicator (port, state, clients) and the Enginewright panel (server on/off, port, token, client configs, clients, tool categories, read-only mode, log level, recent calls, doctor); settings in Project Settings; `editor_capture`.
- Issue forms (tool request, bug report), pull request template, [CONTRIBUTING.md](../CONTRIBUTING.md), [REQUEST_WITH_AI.md](REQUEST_WITH_AI.md) and labelled good first issues.

## Version plan

A new feature raises the second number (0.x.0); a fix only raises the third (0.x.y). The 0.x releases grow with the games that use Enginewright; 1.0.0 is the launch update.

## v0.4.0 (done, current release)

Requests from localizing and play-testing a real game (RagnaCook), then a reliability sweep that ran every tool on it.

- `stringtable_compare` reads a `.po` or `.locres` translation and a CSV source, not only two String Tables; it also reports translations made for an older source text and fuzzy entries.
- `loc_gather` / `loc_gather_status`: gather, export, import, compile and check of a localization target in one call; the commandlet no longer starts an MCP server, so it works while the editor is open; the verdict uses both exit codes.
- `game_run` `culture` (the game process gets the language, not only the editor).
- `font_coverage` (characters of a language that a font cannot draw) and `game_run` `glyphMisses` (characters Slate could not draw in the running game).
- MVVM tools `viewmodel_list`, `viewmodel_add`, `viewmodel_bind`, `viewmodel_unbind`, `viewmodel_remove` (view models and view bindings of a Widget Blueprint, with conversion functions, one undo step, compile and read-back). They live in the optional `EnginewrightMVVM` plugin: Enginewright loads without the MVVM plugin, never enables it, and the tools answer `MVVM_NOT_ENABLED` until both plugins are on.
- `asset_delete` says why an asset cannot be deleted (ER-14): the referencing asset or package, an open editor, unsaved changes; `DELETE_INCOMPLETE` carries a `reason` per asset; the tool text explains `force`.
- `mesh_info` returns local bounds (min, max, center), size and the pivot offsets (from the bounds center and from the base) without spawning an actor (ER-16).
- `blueprint_set_component_defaults` writes defaults of a component inherited from a C++ parent (or an SCS component); `blueprint_info` lists inherited components; `asset_import` takes `scale` (one number or xyz), `rotation`, `translation`, `meshType` for static and skeletal meshes (ER-18).
- `game_run` `shotNames` (ER-20): the run ends with PASSED / `SHOTS_DONE` when each named screenshot (a file name part) is written, `SHOTS_MISSING` when the game ends without one; the tool text spells out the long-run pattern (short `wait`, `STILL_RUNNING`, `game_run_status` loop, two resolutions). `shotsFound` counted a shot copied from another folder twice (ER-09); it now counts each PNG once.
- `image_stats` (ER-19): mean luma, luma histogram (16 bins), saturation, clipped black / white and, with `reference`, the brightness difference, mean absolute pixel difference, changed pixels and SSIM. `game_run` `cvars` (console variables sent as `-ExecCmds`, unknown names refused) and `variants` (the same run once per look, each screenshot with its stats and the diff to the first variant); the tool text lists the scene exposure cvars of UE 5.8.
- Game test tools (ER-21..24). `ui_key`: a key or mouse button through Slate to the focused widget (NativeOnKeyDown, "press any key" panels), with an optional `focus` that moves user focus first and reports what had focus before and after. `pie_walk_to`: walks the pawn to a position with movement input, navmesh path when there is one, `STUCK` / `TIMEOUT` with the last position. `ui_state` lists only what the player can use (visible, top page of its stack, page activated) and counts the rest; `includeHidden` shows it with `layer`, `top` and `hiddenReason`; `ui_click` ignores closed and covered pages, and `AMBIGUOUS` names page and layer of each candidate. `savegame_get` / `savegame_set`: read and write a SaveGame slot by reflection; a slot with a game footer or checksum needs the game's own save function (`saveFunction`).
- Scene dressing tools (ER-15, ER-17). `scene_dress`: ring, ellipse, line, grid or scatter of meshes around an actor or point, one undo step, seed, rotation and scale jitter, ground snap, overlap and avoid checks, instanced or separate actors, DataTable export (relative to the target, same row shape as a game's dressing table). `actor_preview`: build an actor class like the game does (construction, BeginPlay, a few ticks) in an isolated preview world or temporarily in the open level, photograph it from a framed or given camera, clean up.
- CommonUI menu and play-through tools (ER-25..27). `ui_state` buttons carry `text` (every text inside the button, glyphs included) and `tooltip`; `label` is the first text with a letter or digit, and `ui_click` finds a button by any of them. `pie_travel` travels a running session to another map with URL options (`?game=...?listen`), checks the map and the options, and waits for the new map and the pawns. `input_map_list` lists the bound keys per action and device, from an asset or live from the player; `ui_prompts` compares the hints on screen (CommonUI action widgets and key-like texts) with those bindings and answers OK / MISMATCH / PROMPT_NOT_SHOWN / ACTION_NOT_BOUND per `check` entry.
- Reliability sweep: every tool called on the game project (330 calls), 11 fixes; see [RELIABILITY_0.4.md](RELIABILITY_0.4.md).

## v0.4.1 (next): art import pipeline

- FBX import with LODs, collision (UCX), pivot and scale checks.
- Skeleton bind (KayKit Rig_Medium is RagnaCook's base skeleton), IK retarget (for example UE Mannequin clips onto it), sockets, Physics Asset creation.
- Modular character preview.
- Master material template (one parameterised master for a hand-painted style) and texture import settings (sRGB, mask, compression).
- Animation import onto an existing skeleton, Anim Montage and BlendSpace creation, anim notifies.
- Art budget check (triangles and texture size against the game's art direction).
- `asset_find` warns about a folder that does not exist; a warning for asset names with non-ASCII letters.

## v0.5: what the RagnaCook demo still needs

- Behavior Tree / StateTree editing.
- AnimBP state machines.
- Mutable (Customizable Object) tools.

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

## Requests

Requests are GitHub issues with the `request` label (opened with the Tool request form). Each accepted request gets the `roadmap` label and a line here with its issue link and target version; the rest stay open as ideas.

| Issue | Request | Target |
|---|---|---|
| - | No outside requests yet. | - |

Good first issues: [good-first-issues.md](good-first-issues.md).
