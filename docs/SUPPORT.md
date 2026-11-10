# Unreal Engine feature support

This table lists which Unreal Engine features Enginewright can read and write; every row was tested on UE 5.8 Win64 only so far, and other engine versions are planned for 1.0.0 (see [ROADMAP.md](ROADMAP.md)). Status: **full** means read and write tools, tested; **partial** means some tools, with the gaps in Notes; **none** means no tools yet. "Since" is the first Enginewright version with the feature.

| Feature | UE version | Tools | Status | Since | Notes |
|---|---|---|---|---|---|
| Objects and properties | 5.8 | `object_get`, `object_set` | full | 0.1 | `object_set` on a class default also updates placed instances and widget templates that still had the old default (0.3). |
| Assets | 5.8 | `asset_find`, `asset_exists`, `asset_references`, `asset_duplicate`, `asset_rename`, `asset_delete`, `asset_save`, `asset_import` | full | 0.1 | Import runs while the editor is open. `asset_rename` reports text and IDs that still name the old asset. `asset_delete` refuses assets still in use and says why per asset. |
| Enums, structs, DataTables | 5.8 | `enum_create`, `struct_create`, `type_info`, `datatable_create`, `datatable_get`, `datatable_set_rows`, `datatable_remove_rows`, `datatable_rename_row` | full | 0.1 | DataTable tools came in 0.1; enum and struct creation in 0.2. |
| Blueprints (graph editing) | 5.8 | `blueprint_create`, `blueprint_info`, `blueprint_set_component_defaults`, `blueprint_compile`, `blueprint_add_variables`, `blueprint_remove_variables`, `blueprint_add_component`, `blueprint_remove_component`, `blueprint_graph_read`, `blueprint_add_node`, `blueprint_connect`, `blueprint_set_pin`, `blueprint_remove_node` | full | 0.2 | Graph read and edit, variables, components and inherited C++ components. |
| Materials and instances | 5.8 | `material_create`, `material_info`, `material_add_expression`, `material_connect`, `material_instance_create`, `material_instance_set` | full | 0.2 | Material and material instance creation, expressions, connections, parameters. |
| Static / skeletal mesh info and import | 5.8 | `mesh_info`, `mesh_set_material`, `asset_import` | full | 0.2 | `mesh_info` returns local bounds, size and pivot offsets without spawning an actor. Import takes scale, rotation, translation and meshType. |
| Textures | 5.8 | `texture_info`, `asset_import` | full | 0.2 | Import runs while the editor is open. |
| Enhanced Input | 5.8 | `input_action_create`, `input_context_create`, `input_map`, `input_unmap`, `input_context_info`, `input_map_list` | full | 0.2 | `input_map_list` reads from an asset or live from the Play In Editor player, with applied contexts and priorities, runtime remaps included. |
| Levels and actors | 5.8 | `level_open`, `level_save`, `actor_find`, `actor_spawn`, `actor_set_transform`, `actor_delete`, `actor_bounds`, `world_trace`, `class_measure` | full | 0.1 | `level_save` with `path` writes a `.umap` into the project. |
| Scene dressing (scene_dress) | 5.8 | `scene_dress` | full | 0.4 | Ring, ellipse, line, grid or scatter of meshes in one undo step; seed, rotation and scale jitter, ground snap, overlap and avoid checks; instanced or separate actors; DataTable export. |
| Actor preview without PIE (actor_preview) | 5.8 | `actor_preview` | full | 0.4 | Builds an actor class in an isolated preview world or the open level, takes a PNG, cleans up. No Play In Editor, no game_run. |
| Play In Editor (1-4 players) | 5.8 | `pie_start`, `pie_stop`, `pie_state`, `pie_input`, `pie_travel`, `pie_walk_to`, `screenshot`, `screenshot_ab` | full | 0.2 | 1 to 4 players; standalone, listen or client net mode; waits until every player has a pawn. Travel and walk-to came in 0.4. |
| Multiplayer PIE (replication, net emulation) | 5.8 | `pie_actors`, `pie_net_emulation` | full | 0.3 | Server and each client world: count, net roles, owner, property values; waits for replication. Net emulation profiles: off, average, bad. |
| Running the game (-game / packaged exe, game_run) | 5.8 | `game_run`, `game_run_status` | full | 0.3 | `-game` mode or a packaged exe, one run per resolution, verdict from log patterns. Map URL checked against the game's log. `culture`, `cvars`, `variants`, `shotNames` and `glyphMisses` came in 0.4. |
| Screenshots and image checks | 5.8 | `screenshot`, `screenshot_ab`, `image_compose`, `image_stats` | full | 0.2 | `screenshot` and `screenshot_ab` came in 0.2; `image_compose` in 0.3; `image_stats` in 0.4. |
| Save games | 5.8 | `savegame_get`, `savegame_set` | full | 0.4 | Read and write a SaveGame slot by reflection. A slot with a game footer or checksum needs the game's own save function. |
| UMG widgets | 5.8 | `widget_create`, `widget_tree`, `widget_get`, `widget_add`, `widget_set`, `widget_remove`, `widget_render` | full | 0.3 | `widget_render` renders a Widget Blueprint or a C++ UserWidget class to PNG without Play In Editor. |
| Runtime UI and CommonUI | 5.8 | `ui_state`, `ui_key`, `ui_click`, `ui_open`, `ui_close`, `ui_input_type`, `ui_prompts`, `ui_input_audit` | full | 0.3 | CommonUI is optional and used when the project has it. `ui_key`, `ui_prompts` and `pie_travel` support came in 0.4. |
| MVVM (UMG Viewmodel) | 5.8 | `viewmodel_list`, `viewmodel_add`, `viewmodel_bind`, `viewmodel_unbind`, `viewmodel_remove` | partial | 0.4 | Optional plugin. The tools live in `EnginewrightMVVM` and answer `MVVM_NOT_ENABLED` until both plugins are on. |
| Localization (String Tables, GatherText, po / locres) | 5.8 | `loc_gather`, `loc_gather_status`, `stringtable_list`, `stringtable_get`, `stringtable_set`, `stringtable_compare`, `loc_culture` | full | 0.3 | String Table tools came in 0.3; GatherText, `.po` / `.locres` compare and `game_run` `culture` in 0.4. |
| Fonts and glyph coverage | 5.8 | `font_coverage`, `game_run` `glyphMisses` | full | 0.4 | Characters of a language that a font cannot draw; the running game counts glyphs Slate could not draw. |
| Gameplay Tags | 5.8 | `gameplay_tags_list`, `gameplay_tag_add`, `gameplay_tag_remove` | full | 0.3 | Strict names; remove is refused while assets use the tag. |
| C++ build, Live Coding, editor restart | 5.8 | `live_coding_compile`, `editor_restart`, `build_status`, `ewtool.py build` | full | 0.2 | `editor_restart` (close, build, reopen) is Windows-only. `ewtool.py build` runs with the editor closed. |
| Automation tests | 5.8 | `automation_list`, `automation_run` | full | 0.2 | `automation_list` matches anywhere in the name; `automation_run` matches the start. |
| Validation and audits | 5.8 | `validate`, `id_audit`, `key_audit`, `asset_search_text` | full | 0.2 | `id_audit` and `key_audit` answer `ok: false` when they find something. |
| Project settings and log | 5.8 | `settings_get`, `settings_set`, `log_read`, `log_wait`, `console_command`, `editor_state` | full | 0.1 | `settings_set` writes a file in source control. |
| Editor UI (status bar, panel) | 5.8 | `editor_capture`, Enginewright panel and status bar | full | 0.3.1 | Status bar indicator and panel; `editor_capture` takes the editor window or the panel to PNG. |

## Not supported yet

| Feature | UE version | Tools | Status | Since | Notes |
|---|---|---|---|---|---|
| Art import pipeline (FBX / LOD / collision, skeleton bind, IK retarget, sockets, Physics Asset, animation import, Montage / BlendSpace, master material, art budget) | 5.8 | `asset_import`, `mesh_info` | partial | 0.2 | Only `asset_import` (scale, rotation, translation, meshType) and `mesh_info`. The rest is planned for 0.4.1. |
| Behavior Tree / StateTree | 5.8 | none | none | n/a | Planned for 0.5. |
| Animation Blueprints (state machines) | 5.8 | none | none | n/a | Planned for 0.5. |
| Mutable (Customizable Object) | 5.8 | none | none | n/a | Planned for 0.5. |
| Niagara | 5.8 | none | none | n/a | Planned for 1.0.0. |
| Sequencer | 5.8 | none | none | n/a | Planned for 1.0.0. |
| Headless mode (editor closed) | 5.8 | none | none | n/a | Planned for 1.0.0. Only `ewtool.py build` works with the editor closed today. |

Missing something? Use the [Tool request form](https://github.com/shanyup/enginewright/issues/new?template=feature_request.yml).
