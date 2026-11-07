# Enginewright

An Unreal Engine editor plugin that runs a [Model Context Protocol](https://modelcontextprotocol.io) (MCP) server inside the editor, so AI coding agents (Claude Code, Codex, Cursor, VS Code, Hermes or any MCP client) can read and edit your project. Every write is validated first and read back after.

> **Early preview (v0.2).** One maintainer, APIs may change. Use it on a project under version control and review what it changes.

## Why

The focus is on edits you can trust:

- **Nothing extra to install.** One native C++ editor plugin. No Python, Node, proxy process or Epic's experimental MCP plugin.
- **Strict validation.** Unknown arguments, unknown properties or struct fields, wrong types and bad enum names are errors with a suggestion. They are never ignored.
- **All or nothing.** If one value in a call is bad, nothing is written. Each write is one undo step and is read back and compared.
- **Rollback.** `batch` runs several steps in one call; if a step fails, the earlier steps are undone.
- **Safe by default.** Asset edits are refused while Play In Editor runs. Deleting an asset that is still used is refused, and the reply lists who uses it. The server listens on localhost only and needs a per-project token.

### Compared with Epic's MCP toolset in UE 5.8

UE 5.8 ships an experimental MCP toolset with a larger tool catalog than Enginewright. Enginewright makes a different trade-off. In our use of the 5.8 toolset, some writes reported success while a misspelled key was silently ignored, and some asset lookups gave misleading results during Play In Editor. Enginewright rejects those cases with an error. Epic's toolset exists only in 5.8; Enginewright does not depend on it.

## Supported versions

| Unreal Engine | Platform | Status |
|---|---|---|
| 5.8 | Windows (Win64) | Tested (prebuilt zip and source) |
| Other versions, Mac, Linux | — | Not tested yet |

## Install (about 5 minutes)

**Prebuilt zip (Blueprint or C++ projects, UE 5.8 Win64)**

1. Download `Enginewright-<version>-UE5.8-Win64.zip` from [Releases](https://github.com/shanyup/enginewright/releases).
2. Close the editor. Unzip into your project's `Plugins/` folder, so you get `<YourProject>/Plugins/Enginewright/Enginewright.uplugin`.
3. Open the project. The plugin is enabled by default. The Output Log prints
   `Enginewright: MCP server on http://127.0.0.1:3100/mcp`.
4. Connect your client. Ready-to-paste configs (URL + token) are in `<YourProject>/Saved/Enginewright/clients/`:

   | Client | File | What to do |
   |---|---|---|
   | Claude Code | `claude-code.txt` | Run the command once in your project folder (or merge `claude-code.mcp.json` into `.mcp.json`). |
   | Codex | `codex.toml` | Append to `~/.codex/config.toml`. |
   | Cursor | `cursor.mcp.json` | Merge into `.cursor/mcp.json`. |
   | VS Code | `vscode.mcp.json` | Merge into `.vscode/mcp.json`. |
   | Hermes | `hermes.yaml` | Merge under `mcp_servers` in your Hermes `config.yaml`. |

   The files hold your project's token: do not commit them (`Saved/` is normally ignored). An agent that is already connected can print them with the `client_config` tool.
5. Ask your agent to call `doctor`. It reports the engine version, the server, the open level and any problems.

**From source (C++ projects, or to build it yourself)**

1. Copy `Plugins/Enginewright` into your project's `Plugins/` folder.
2. Build your project's editor target (or open the `.uproject` and accept the rebuild prompt).
3. Continue with step 3 above.

To use another port, set the `ENGINEWRIGHT_PORT` environment variable before starting the editor.

## Tools

The client sees three meta tools: `search` (find a tool in plain words), `describe` (read its arguments) and `call` (run it). Behind them:

| Area | Tools |
|---|---|
| Objects | `object_get`, `object_set` |
| Assets | `asset_find`, `asset_exists`, `asset_references`, `asset_duplicate`, `asset_rename` (updates referencers, removes the redirector, reports text and IDs that still name the old asset; `dryRun` to preview), `asset_delete` (refuses assets still in use), `asset_save`, `asset_import` (texture, sound, font, mesh, while the editor is open) |
| Types and data | `enum_create`, `struct_create`, `type_info`, `datatable_create`, `datatable_get`, `datatable_set_rows`, `datatable_remove_rows`, `datatable_rename_row` |
| Blueprints | `blueprint_create`, `blueprint_info`, `blueprint_compile`, `blueprint_add_variables`, `blueprint_remove_variables`, `blueprint_add_component`, `blueprint_remove_component`, `blueprint_graph_read`, `blueprint_add_node`, `blueprint_connect`, `blueprint_set_pin`, `blueprint_remove_node` |
| Materials, meshes, textures | `material_create`, `material_info`, `material_add_expression`, `material_connect`, `material_instance_create`, `material_instance_set`, `mesh_info`, `mesh_set_material`, `texture_info` |
| Enhanced Input | `input_action_create`, `input_context_create`, `input_map`, `input_unmap`, `input_context_info` |
| Level | `level_open`, `level_save`, `actor_find`, `actor_spawn`, `actor_set_transform`, `actor_delete`, `actor_bounds`, `world_trace`, `class_measure` (size of a class or Blueprint without placing it) |
| Play In Editor | `pie_start` (1-4 players; standalone, listen or client; waits until every player has a pawn), `pie_stop`, `pie_state`, `pie_input` (synthetic Enhanced Input action or key per player), `screenshot` (what a player sees, or the editor viewport), `screenshot_ab` (same view before and after a change, with the share of changed pixels) |
| Multiplayer | `pie_actors` (what the server and each client see: count, net roles, owner, property values; can wait for replication), `pie_net_emulation` (lag, loss, duplicates, reordering; profiles off / average / bad) |
| Readability | `screen_coverage` (how big an actor looks from a player's camera: pixels and share of the screen) |
| Build and tests | `live_coding_compile`, `editor_restart` (save, close, optionally build C++, reopen), `build_status`, `automation_list`, `automation_run` |
| Checks | `validate` (compile Blueprints, data validation, broken references, map check), `id_audit` (IDs used but never defined, and the reverse), `key_audit` (hard-coded input keys in C++ and Blueprints), `asset_search_text` |
| Editor | `editor_state`, `console_command`, `log_read`, `log_wait` (wait for a log line), `settings_get`, `settings_set`, `doctor`, `client_config` |
| Multi-step | `batch` (atomic, rolls back on failure) |

## Running the tests

The tests in `tests/` are plain Python 3 scripts (standard library only). They talk to a running editor. Use a separate, empty test project: the suites create and delete assets under `/Game/_EWTest` and add (then remove) actors in the open level. They create their own fixtures, so a new blank project works.

```bash
# bash / zsh
export ENGINEWRIGHT_TEST_PROJECT="/path/to/TestProject"
# PowerShell
$env:ENGINEWRIGHT_TEST_PROJECT = "D:\Projects\TestProject"

python tests/run_all.py             # every suite except build
python tests/run_all.py smoke core  # only these suites
python tests/run_all.py build       # Live Coding, automation tests, full C++ rebuild (restarts the editor)
```

The tests read the token from `<project>/Saved/Enginewright/token`. If you changed the port, also set `ENGINEWRIGHT_URL` (default `http://127.0.0.1:3100/mcp`). `tests/smoke.py` is read-only and safe on any project.

## Building a release zip

```bash
python tools/package.py --engine "C:/Program Files/Epic Games/UE_5.8"
```

It runs `RunUAT BuildPlugin` for Win64, checks the result (editor DLL present, no debug or intermediate files) and writes `dist/Enginewright-<version>-UE5.8-Win64.zip` with its SHA-256.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md).

## License

[MIT](LICENSE)
