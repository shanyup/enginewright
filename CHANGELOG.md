# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]
- 0.4.1: art pipeline (FBX import with LODs, collision and pivot / scale checks; skeleton bind, IK retarget, sockets, Physics Asset; modular character preview; master material template and texture import settings; animation import, Anim Montage and BlendSpace, anim notifies; art budget check; `asset_find` and non-ASCII asset name warnings). See docs/ROADMAP.md.

## [0.4.0] - 2026-11-10
Release notes: [docs/release-notes/v0.4.0.md](docs/release-notes/v0.4.0.md)

### Added
- Localization: `stringtable_compare` reads a `.po`, `.locres` or CSV translation, not only two String Tables, with translations of an older source text and fuzzy entries.
- Localization: `loc_gather` / `loc_gather_status` take a target through gather, export, import, compile and check in one call, with the verdict from both exit codes.
- Localization: `game_run` `culture` starts the game process with a language, not only the editor.
- Localization: `font_coverage` and `game_run` `glyphMisses` find characters a font or Slate cannot draw.
- MVVM: `viewmodel_list`, `viewmodel_add`, `viewmodel_bind`, `viewmodel_unbind` and `viewmodel_remove` in the optional `EnginewrightMVVM` plugin.
- `image_stats`: mean luma, luma histogram (16 bins), saturation, clipped black / white and the difference to a reference image.
- `game_run` `shotNames`, `cvars` and `variants`.
- Game testing: `ui_key`, `pie_walk_to`, `pie_travel`, `input_map_list`, `ui_prompts`.
- Save games: `savegame_get` / `savegame_set`.
- Scene tools: `scene_dress`, `actor_preview`.
- Editor panel: the version label and the "What's new / Roadmap" and "Changelog" buttons.
- `docs/SUPPORT.md` (Unreal feature support matrix), `CHANGELOG.md`, the roadmap Requests section and the request flow in CONTRIBUTING.md.
- GitHub Discussions (Announcements, Ideas, Q&A, Show and tell); the issue chooser links questions and rough ideas there.
- Release: the optional MVVM plugin as its own zip, `EnginewrightMVVM-0.4.0-UE5.8-Win64.zip`.

### Changed
- `ui_state` lists only visible widgets on the top page of their stack (visible, page activated, button text, tooltip and label); `includeHidden` adds the rest.
- `ui_click` finds a button by name, label, any text inside it or tooltip, and ignores closed and covered pages.
- `asset_delete` says why an asset cannot be deleted, per asset.
- `mesh_info` returns local bounds, size and the pivot offsets without spawning an actor.
- `blueprint_set_component_defaults` writes defaults of an inherited component.
- `asset_import` takes `scale`, `rotation`, `translation` and `meshType`.
- Every tool argument has a type and a description.

### Fixed
- `game_run` `shotsFound` counted a screenshot copied from another folder twice (ER-09); it now counts each PNG once.
- `datatable_set_rows` checks rows on copies first, so a failed call no longer overwrites every row's `Name` field.
- `level_save` with `path` writes the `.umap` inside `Content/` and registers it with the asset registry.
- `asset_delete` deletes a set of assets that only use each other, and String Tables without `force`.
- `gameplay_tag_add` / `gameplay_tag_remove` remove a tag ini they created when its last tag is removed.
- `game_run` moves screenshots from `Saved/Screenshots` into the run folder instead of copying them.
- `game_run` reports `MAP_NOT_FOUND` for a `/Game` map in `url` that does not exist, also in `dryRun`.
- `game_run` `dryRun` heavy-level warning matches what happens (viewports pause by default).
- `stringtable_compare` uses the CSV file name as namespace when the `.po` has that namespace.
- 80 arguments had no description and 8 had no type; every argument now has a type and a description.
- `key_audit`, `id_audit`, `automation_list`, `automation_run`, a stopped `game_run` and `settings_set` describe their real behaviour.
- `tests/smoke.py` with no editor prints one line instead of a urllib traceback.

## [0.3.1] - 2026-11-09
Release notes: [docs/release-notes/v0.3.1.md](docs/release-notes/v0.3.1.md)

### Added
- Status bar indicator (`Enginewright :3100 (n)`) and the Enginewright panel (server on/off, port, token, client configs, clients, tool categories, read-only mode, log level, recent calls, `doctor`).
- Settings in Project Settings > Plugins > Enginewright, saved per user and project.
- `editor_capture` writes the editor window or the Enginewright panel to a PNG.
- Issue forms, pull request template, [CONTRIBUTING.md](CONTRIBUTING.md), [docs/REQUEST_WITH_AI.md](docs/REQUEST_WITH_AI.md) and labelled good first issues.

### Fixed
- `widget_render` also renders C++ UserWidget classes, not only Widget Blueprints.
- `game_run` refuses to start after a Live Coding patch and warns when C++ source changed after the last build.
- `game_run` pauses the editor's realtime level viewports and reports a heavy editor level.
- `game_run` `shotsExpected` ends a screenshot run when the PNG files are written, with a `PASSED` verdict.

## [0.3.0] - 2026-11-08
Release notes: [docs/release-notes/v0.3.0.md](docs/release-notes/v0.3.0.md)

### Added
- UMG: `widget_create`, `widget_tree`, `widget_get`, `widget_add`, `widget_set`, `widget_remove` and `widget_render`.
- Runtime UI with CommonUI: `ui_state`, `ui_click`, `ui_open`, `ui_close`, `ui_input_audit`, `ui_input_type`.
- Localization: `stringtable_list`, `stringtable_get`, `stringtable_set`, `stringtable_compare`, `loc_culture`.
- `game_run` / `game_run_status`, `image_compose`.
- Gameplay Tags: `gameplay_tags_list`, `gameplay_tag_add`, `gameplay_tag_remove`.
- Multiplayer tests: `pie_actors`, `pie_net_emulation`.
- `class_measure`, `screen_coverage`.
- Optional `Scripts/ewtool.py` build helper for the one job outside the editor.

### Changed
- `asset_rename` reports text and IDs that still name the old asset; `dryRun` previews them.
- `object_set` on a class default also updates placed instances and widget templates that still had the old default.
- `automation_list` / `automation_run` keep the editor at full speed in the background.
- `screenshot` of Play In Editor includes the UI, as the player sees it; `ui: false` captures the scene only.
- Every tool argument has a description in `describe`.

### Fixed
- The plugin builds in projects that use unity builds.
- Adding and removing widgets keeps the Widget Blueprint's variable list consistent.

## [0.2.0] - 2026-11-07
Release notes: [docs/release-notes/v0.2.0.md](docs/release-notes/v0.2.0.md)

### Added
- Prebuilt UE 5.8 Win64 zip and ready-to-paste client configs for Claude Code, Codex, Cursor, VS Code and Hermes (`client_config`).
- `screenshot`, `screenshot_ab`.
- Play In Editor control: `pie_start` (1-4 players, standalone / listen / client), `pie_input`, `log_wait`.
- Build loop: `live_coding_compile`, `editor_restart`, `build_status`, `automation_list`, `automation_run`.
- Blueprint, Material and Material Instance, mesh and texture tools; enum and struct creation.
- Import while the editor is open (texture, sound, font, mesh), Enhanced Input helpers, validation.
- Content checks: `id_audit`, `key_audit`.
- `level_open`, `level_save`.
- Test suites that build their own fixtures (`tests/run_all.py`).

### Fixed
- Deleting an asset and creating it again at the same path in one editor session works.
- Renaming an asset no longer redirects a new asset created later at the old path.
- Blueprint enum variables accept the enumerator's display name as default value; an unknown name is an error with a suggestion.

## [0.1.0] - 2026-11-06

### Added
- In-editor HTTP MCP server with token and the three meta tools `search`, `describe` and `call`.
- Strict validation with read-back.
- Object, asset, DataTable, level, editor, project settings and log tools.
- `doctor`.
- Atomic `batch` with rollback.

[Unreleased]: https://github.com/shanyup/enginewright/compare/v0.4.0...dev
[0.4.0]: https://github.com/shanyup/enginewright/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/shanyup/enginewright/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/shanyup/enginewright/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/shanyup/enginewright/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/shanyup/enginewright/releases/tag/v0.1.0
