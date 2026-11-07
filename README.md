# Enginewright

An Unreal Engine editor plugin that runs a [Model Context Protocol](https://modelcontextprotocol.io) (MCP) server inside the editor, so AI coding agents (Claude Code, Codex, Cursor or any MCP client) can read and edit your project. Every write is validated first and read back after.

> **Early preview (v0.1).** Small tool set, one maintainer, APIs may change. Use it on a project under version control and review what it changes.

## Why

The focus is on edits you can trust:

- **Nothing extra to install.** One native C++ editor plugin. No Python, Node, proxy process or Epic's experimental MCP plugin.
- **Strict validation.** Unknown arguments, unknown properties or struct fields, wrong types and bad enum names are errors with a suggestion. They are never ignored.
- **All or nothing.** If one value in a call is bad, nothing is written. Each write is one undo step and is read back and compared.
- **Rollback.** `batch` runs several steps in one call; if a step fails, the earlier steps are undone.
- **Safe by default.** Asset edits are refused while Play In Editor runs. The server listens on localhost only and needs a per-project token.

### Compared with Epic's MCP toolset in UE 5.8

UE 5.8 ships an experimental MCP toolset with a much larger tool catalog than Enginewright. Enginewright is smaller and makes a different trade-off. In our use of the 5.8 toolset, some writes reported success while a misspelled key was silently ignored, and some asset lookups gave misleading results during Play In Editor. Enginewright rejects those cases with an error. Epic's toolset exists only in 5.8; Enginewright does not depend on it.

## Supported versions

| Unreal Engine | Platform | Status |
|---|---|---|
| 5.8 | Windows (Win64) | Tested |
| Other versions, Mac, Linux | — | Not tested yet |

## Install

1. Copy `Plugins/Enginewright` into your project's `Plugins/` folder.
2. Build the project. The plugin ships as C++ source; there are no prebuilt binaries yet.
3. Open the editor. The log prints `Enginewright: MCP server on http://127.0.0.1:3100/mcp`.
   To use another port, set the `ENGINEWRIGHT_PORT` environment variable before starting the editor.
4. Add the server to your MCP client. A ready config (URL + token header) is written to
   `Saved/Enginewright/client-config.json`. The token is in `Saved/Enginewright/token`; do not commit it.

## Tools

The client sees three meta tools: `search` (find a tool in plain words), `describe` (read its arguments) and `call` (run it). Behind them:

| Area | Tools |
|---|---|
| Objects | `object_get`, `object_set` |
| Assets | `asset_find`, `asset_exists`, `asset_references`, `asset_duplicate`, `asset_rename` (fixes up redirectors), `asset_delete` (refuses referenced assets), `asset_save` |
| DataTables | `datatable_create`, `datatable_get`, `datatable_set_rows`, `datatable_remove_rows`, `datatable_rename_row` |
| Level | `actor_find`, `actor_spawn`, `actor_set_transform`, `actor_delete`, `actor_bounds`, `world_trace` |
| Editor | `editor_state`, `console_command`, `pie_start`, `pie_stop`, `log_read`, `doctor` |
| Project settings | `settings_get`, `settings_set` |
| Multi-step | `batch` (atomic, rolls back on failure) |

## Running the tests

The tests in `tests/` are plain Python 3 scripts (standard library only). They talk to a running editor, so open your project with the plugin first.

Set the project folder once per shell. The tests read the token from `<project>/Saved/Enginewright/token`.

```bash
# bash / zsh
export ENGINEWRIGHT_TEST_PROJECT="/path/to/MyProject"
# PowerShell
$env:ENGINEWRIGHT_TEST_PROJECT = "D:\Projects\MyProject"
```

If you changed the port, also set `ENGINEWRIGHT_URL` (default `http://127.0.0.1:3100/mcp`).

- `python tests/smoke.py [project_dir] [blueprint_path]` checks auth, the meta tools and basic calls. It works on any project. With a Blueprint path it also checks `object_get` / `object_set` on that Blueprint's defaults.
- `python tests/v01_tests.py` runs the full v0.1 tool test. It writes only under `/Game/_MCPTest` and adds a test light to the open level, then removes it. It needs fixtures that are not in this repo yet: a Blueprint at `/Game/_MCPTest/BP_UEMCPTest` with `ItemID` and `Weight` (float) variables, and a row struct `FItemDefinition` with `DisplayName`, `Category` (enum with `RawIngredient` and `Dish`), `ItemClass` (soft class) and `ProcessedItemID`.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md).

## License

[MIT](LICENSE)
