# Good first issues (drafts)

Drafts to open on GitHub once the maintainer approves. Each is small and has a test to extend.

## 1. Test on UE 5.7 (and report what breaks)

Build the plugin from source in a blank UE 5.7 C++ project, run `python tests/run_all.py` and list compile errors or failing checks. Fixes guarded with `ENGINE_MINOR_VERSION` are welcome.

## 2. `asset_find`: filter by tag value

Add an optional `tags` object (`{"TagName": "value"}`) that keeps only assets whose asset-registry tag matches. Extend `tests/test_core.py` with one positive and one unknown-tag case.

## 3. `datatable_get`: CSV / JSON export

Add `format: "csv" | "json"` and return the table as text (the editor already has exporters). Add a round-trip check to `tests/test_core.py`.

## 4. `screenshot`: optional resolution

Let `screenshot` take `width` and `height` (validated, 64-4096) and check the PNG size in `tests/test_pie.py`.

## 5. Docs: one page per client

Short setup pages for Claude Code, Codex, Cursor, VS Code and Hermes under `docs/clients/`, using the files in `Saved/Enginewright/clients/`.
