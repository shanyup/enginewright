# Contributing to Enginewright

Thanks for helping. Enginewright is an early preview kept by one maintainer, so reviews can take a few days. Small, focused pull requests with tests are the fastest to merge.

Ways to help:

- **Request a tool** with the [Tool request form](https://github.com/shanyup/enginewright/issues/new?template=feature_request.yml). Your own AI agent can fill it in: see [docs/REQUEST_WITH_AI.md](docs/REQUEST_WITH_AI.md).
- **Report a bug** with the [Bug report form](https://github.com/shanyup/enginewright/issues/new?template=bug_report.yml). Include the `doctor` output.
- **Pick a [good first issue](https://github.com/shanyup/enginewright/labels/good%20first%20issue).** Each one names the files to touch, the test to extend and when it is done.
- **Send a pull request.** For a new tool, open a request first so we agree on the name and arguments before you write code.

## Development setup

You need Unreal Engine 5.8 on Windows (the only tested platform so far), a Visual Studio version that UE 5.8 supports, with the C++ game development workload (the maintainer builds with Visual Studio 2026), Git, and Python 3 for the tests (standard library only).

1. Fork and clone the repository.
2. Create a separate, empty **C++** test project in UE 5.8 (File > New Project > Games > Blank, C++). Do not use a real game project: the tests create and delete assets under `/Game/_EWTest` and add actors to the open level.
3. Link the plugin source into the test project, so a build of the test project builds your working copy. In a Command Prompt:

   ```bat
   mkdir "D:\Projects\EWTest\Plugins"
   mklink /J "D:\Projects\EWTest\Plugins\Enginewright" "D:\src\enginewright\Plugins\Enginewright"
   ```

   (Copying the folder works too, but then you have to copy again after every change.)
4. Enable the **CommonUI** plugin in the test project (Edit > Plugins), for the CommonUI checks of the `ui` suite.
5. Optional, for the C++ widget check of the `umg` suite: copy `tests/fixtures/EWTestNativeWidget.h` and `.cpp` into the test project's module folder (`Source/<Module>/`) and add `"UMG", "Slate", "SlateCore"` to that module's `Build.cs` dependencies. The test looks for `/Script/EnginewrightTest.EWTestNativeWidget`; if your module has another name, change the path in `tests/test_umg.py` locally.
6. Optional, for the `mvvm` suite: copy `tests/fixtures/EWTestViewModel.h` and `.cpp` into the same module, add `"ModelViewViewModel"` and `"FieldNotification"` to its `Build.cs` dependencies, copy `Plugins/EnginewrightMVVM` next to `Plugins/Enginewright` and enable `ModelViewViewModel` and `EnginewrightMVVM` in the `.uproject`. Without them the suite only checks that the tools answer `MVVM_NOT_ENABLED`.
7. Build the editor target with the editor closed, either by opening the `.uproject` and accepting the rebuild prompt, or from a shell:

   ```bat
   "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" EWTestEditor Win64 Development -Project="D:\Projects\EWTest\EWTest.uproject" -WaitMutex
   ```

   `Plugins/Enginewright/Scripts/ewtool.py build --project <uproject>` does the same and also waits for locked DLLs (see [docs/EWTOOL.md](docs/EWTOOL.md)).
8. Open the test project. The Output Log prints `Enginewright: MCP server on http://127.0.0.1:3100/mcp (… tools)`.

The editor locks the plugin DLL. After a C++ change, close the editor and build again, or call `editor_restart` with `{"build": true}` from your agent. Live Coding is fine for quick edits in `.cpp` files, but header changes need a full build.

## How the code is laid out

| Path | What |
|---|---|
| `Plugins/Enginewright/Source/Enginewright/Private/Tools/<Area>Tools.cpp` | The tools, one file per area (UMG, PIE, game run, ...). Each file has one `Register<Area>Tools(FEWRegistry&)` function. |
| `Private/EnginewrightModule.cpp` | Starts the HTTP server and calls every `Register<Area>Tools` after `SetCategory(...)` (the category groups tools in the panel and lets a project turn them off). A new tool file needs one declaration and one call there. |
| `Private/EWRegistry.cpp` | The shared checks for every call: unknown arguments, types, enums, required arguments, the Play In Editor gate, undo transactions. |
| `Private/EWCommon.h`, `EWProps.h`, `EWTypes.h` | Helpers: argument readers (`EW::Str`, `EW::Num`, `EW::Bool`, `EW::StrList`), asset and class lookup (`EW::LoadAs`, `EW::FindClass`, `EW::Suggest`), property writing with read-back (`EW::WriteValues`), log capture, PNG encoding. |
| `Private/EWEditorUI.cpp`, `Public/EnginewrightSettings.h` | The status bar indicator, the Enginewright panel (Tools > Enginewright) and the settings behind them (Project Settings > Plugins > Enginewright). |
| `Plugins/EnginewrightMVVM/` | The optional second plugin with the code that touches Epic's MVVM plugin (the `viewmodel_*` operations). The tools, schemas and descriptions stay in `Plugins/Enginewright/.../Tools/MVVMTools.cpp`; this plugin registers its operations through `FEWMVVMBackend`, so the main module has no dependency on MVVM. |
| `Plugins/Enginewright/Scripts/ewtool.py` | Optional build helper for when the editor is closed. |
| `tests/` | Python test suites, one file per area (`test_<suite>.py`), run against a live editor. |
| `tools/package.py` | Builds the release zip. |

## Rules for tools

Every tool follows these rules. Reviews check them.

1. **Strict validation.** The schema lists every argument with a `type` and a one-line `description`. The registry rejects unknown arguments, wrong types and values outside an `enum` before your code runs. Check the rest yourself (ranges, paths, names) and return an error with the closest valid names (`EW::Suggest`). Never ignore a bad value.
2. **All or nothing.** Validate everything first, then write. If one value is bad, nothing changes.
3. **Read-back.** After a write, read the values back from the object (not from the input) and return them, so the agent sees what is really there.
4. **One undo step.** A tool that edits objects opens one `FScopedTransaction` for the whole call and calls `Modify()` before each change, so one Ctrl+Z reverts it. If the call fails after the transaction started, cancel it with `EW::CancelUnlessBatch(Tx)` (see `object_set` in `ObjectTools.cpp`). Steps an undo cannot revert (creating, renaming, deleting or saving assets) pass `/*bTransactional*/ false` to `Register`; `batch` then refuses them in an atomic batch.
5. **No writes during Play In Editor.** `bWrites = true` tools are refused while PIE runs (`PIE_RUNNING`). Read-only tools set `bWrites = false`.
6. **Clear replies.** Error codes are `UPPER_SNAKE_CASE` (`ASSET_NOT_FOUND`, `INVALID_ARGUMENT`, ...). Messages are short, plain English and say how to fix the problem.
7. **The description is the documentation.** Agents only see the tool description and the schema (`describe`). Say what the tool does, what it changes, and when to use another tool instead. Mention the words people will search for (`search` matches on them).
8. **Names.** `area_verb` in snake_case, like the existing tools (`widget_add`, `pie_input`, `stringtable_compare`).

A small read-only tool looks like this:

```cpp
R.Register(TEXT("example_count_actors"),
	TEXT("Count the actors of a class in the open level. Read only."),
	TEXT(R"({"type":"object","properties":{
		"class":{"type":"string","description":"Class name, /Script path or Blueprint path, e.g. StaticMeshActor."}},
		"required":["class"]})"),
	/*bWrites*/ false,
	[](const TSharedPtr<FJsonObject>& Args) -> FEWResult
	{
		FString Err;
		TArray<FString> Sug;
		UClass* Class = EW::FindClass(EW::Str(Args, TEXT("class")), Err, &Sug);
		if (!Class) { return FEWResult::Error(TEXT("UNKNOWN_CLASS"), Err, Sug); }
		UWorld* World = GEditor->GetEditorWorldContext().World();
		int32 Count = 0;
		for (TActorIterator<AActor> It(World, Class); It; ++It) { ++Count; }
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("count"), Count);
		return FEWResult::Ok(Data);
	});
```

(It needs `#include "Editor.h"` and `#include "EngineUtils.h"` in the tool file.) Tools that finish on a later frame (waiting for a log line, running the game) use `R.RegisterAsync` and call `Done(...)` once.

## Tests

Every new tool and every bug fix comes with tests in the suite of its area (`tests/test_<area>.py`). A test file:

- talks to the running editor through `tests/ewtest.py` (`call`, `check`, `done`, `delete_if_exists`, `fixtures`);
- creates its own fixtures under `/Game/_EWTest` with Enginewright's own tools, so it runs on a blank project;
- checks the success case **and** each error code the tool returns;
- checks the result independently where it can (read the asset or file back, decode the PNG), not only the tool's reply;
- deletes everything it created, so a second run starts clean.

Run the tests with the test project open in the editor:

```bash
# bash
export ENGINEWRIGHT_TEST_PROJECT="D:/Projects/EWTest"
# PowerShell
$env:ENGINEWRIGHT_TEST_PROJECT = "D:\Projects\EWTest"

python tests/run_all.py             # every suite except build
python tests/run_all.py umg game    # only these suites
python tests/run_all.py build       # Live Coding, automation tests, full C++ rebuild (restarts the editor)
python tests/smoke.py               # read-only, safe on any project
```

Each suite prints one line per check and a `N/M passed` summary; `run_all.py` prints one line per suite and exits with 0 only when all of them passed. Set `ENGINEWRIGHT_URL` if you changed the port, and `ENGINEWRIGHT_TEST_GAME=1` to include the slow checks that start the real game (`game` suite).

## Commits and pull requests

1. Branch from `main`. Keep one topic per pull request.
2. Write the commit subject in plain English, saying what changed for the user (`widget_render: also C++ UserWidget classes`).
3. Run the full test suite (and `build` if you touched the build tools) and paste the `run_all.py` summary into the pull request template.
4. Update the README tool table and `docs/ROADMAP.md` when the tool list changes.
5. The maintainer may test your change on another project before merging and may ask for changes. Releases are cut from `main` with release notes and a prebuilt zip.

By contributing you agree that your contribution is licensed under the [MIT License](LICENSE).

## Code style

- C++: Unreal Engine style (tabs, `F`/`U`/`A` prefixes, braces on their own line; short one-line blocks are fine, as in the existing files). Helpers private to a file go in an anonymous namespace; the module builds without unity, so names may repeat across files.
- Keep a file focused on its area. If it grows past about 1000 lines, split it.
- No new plugin dependencies without a reason in the pull request (the plugin should load on a blank project).
- Python (tests, tools): Python 3 standard library only, no extra packages.
- Text that users or agents read (descriptions, errors, docs): short sentences, plain English, no promises.
