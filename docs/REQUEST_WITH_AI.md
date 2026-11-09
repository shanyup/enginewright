# Request a tool with your own AI

Enginewright grows from real needs. If your agent could not do an editor task with the existing tools, your agent is also the best one to describe what is missing: it knows the task, and it can check Enginewright's tool list itself.

1. Start your agent (Claude Code, Codex, Cursor, VS Code, Hermes, ...) with Enginewright connected and your project open in the editor.
2. Paste the prompt below. Replace the line after `TASK:` with what you wanted to do.
3. The agent searches the existing tools first. If one of them already does the job, it tells you how to call it, and there is nothing to request.
4. Otherwise it writes a filled-in request. Open a [Tool request](https://github.com/shanyup/enginewright/issues/new?template=feature_request.yml) and copy each section into the field with the same name.
5. Read it before you send it: remove project names or paths you do not want to make public. Never paste your token or client config files.

## The prompt

````text
You are connected to Enginewright, an MCP server inside the Unreal Engine editor. It exposes three meta tools:
search (find a tool in plain words), describe (read a tool's arguments) and call (run a tool).

TASK: <what I wanted the agent to do in the editor, in one or two sentences>

Do this:
1. Call doctor once and note the engine version and Enginewright version.
2. Call search with 2-4 different phrasings of the task (short, plain words). Call describe on every
   tool that looks related. If it helps, try read-only calls (never write to my project for this).
3. Decide: can the task be done with the existing tools, alone or combined (batch)?
   - If yes: show me the exact calls and stop. Do not write a request.
   - If only partly: say which part works with which tool, and write the request only for the missing part.
4. Write the request in Markdown with exactly these headings, in this order, so I can copy each one into
   the GitHub form field of the same name:

   ### What should the agent be able to do?
   A user story: "As a ..., I want my agent to ..., so that ...".
   ### How is it done today?
   What I do by hand in the editor now, and which Enginewright tools you tried (search words + results)
   and why they are not enough.
   ### Proposed tool name
   One snake_case name, area first, in the style of the existing tools (e.g. widget_add, pie_input).
   ### Input (JSON schema)
   A JSON schema object: every argument with type and a one-line description; required list.
   Add dryRun when the tool writes.
   ### Expected output
   A JSON example of a successful reply, including the values read back after a write,
   and the error codes it should return (e.g. ASSET_NOT_FOUND, INVALID_ARGUMENT).
   ### Does it change the project?
   One of: Read only (changes nothing) / Writes assets or the level /
   Writes files outside Content (config, source, Saved) / Runs something (Play In Editor, the game, a build)
   ### Can the change be undone?
   One of: Yes, one undo step / No (creates, renames, deletes or saves assets) / Not applicable (read only)
   ### Unreal Engine version
   From doctor.
   ### Example in a project
   A small concrete case, best one that works on a new blank project (asset paths, classes, level content).
   ### Acceptance test
   Numbered steps "call -> expected result", including at least one error case where nothing is written.
   ### Anything else
   Engine classes or APIs you know are involved, plugins it depends on.

Rules: plain English, short sentences. Use only tool names that search/describe really returned.
Do not invent results of calls you did not make. Keep my project's private names out unless I agree.
````

## Example

This is a real run of the prompt above by Claude (Enginewright 0.3.1, UE 5.8.4, a blank test project without the MVVM plugin), unedited. Task given to the agent: *"Bind a TextBlock in my shop widget to a property of an MVVM view model."*

The agent called `doctor`, then `search` six times ("view model", "MVVM binding", "MVVM", "viewmodel", "bind widget property to view model", "widget binding") and `describe` on `widget_set`, `widget_get` and `object_set`. It found that the widget itself can be built with existing tools, and wrote a request only for the missing part:

<details>
<summary>The agent's full reply</summary>

> Checked with doctor: Unreal Engine 5.8.4, Enginewright 0.3.1.
>
> Searches: "view model" (no binding tool; screenshot/UI tools only), "MVVM binding" (no match), "MVVM" (no match),
> "viewmodel" (no match), "bind widget property to view model" (widget_add, widget_get, widget_render, ...),
> "widget binding" (widget_create, widget_tree, widget_get, widget_add, widget_set, widget_remove, widget_render, ui_state).
> Described widget_set, widget_get and object_set: none of them has an argument for a view model or a binding.
>
> Can it be done with existing tools? Partly. widget_create / widget_add can build the widget (a TextBlock named Gold),
> and widget_set can set a fixed Text. Adding a view model to the Widget Blueprint and binding Text to a view model
> property is missing, so the request below covers only that part.
>
> ### What should the agent be able to do?
> As a developer who uses UMG with the MVVM plugin (UMG Viewmodel), I want my agent to add a view model to a Widget Blueprint and bind a widget property to a view model property, so that it can wire a screen without me opening the View Bindings panel.
>
> ### How is it done today?
> By hand in the Widget Blueprint editor: Window > Viewmodels to add the view model, then Window > View Bindings to add each binding.
> Tried with Enginewright: search "view model", "MVVM binding", "MVVM", "viewmodel" (no binding tool, three searches had no match), "bind widget property to view model" and "widget binding" (widget_add, widget_get, widget_set, widget_tree, widget_create). describe on widget_set, widget_get and object_set: they set or read fixed property values only; there is no argument for a view model or a binding.
>
> ### Proposed tool name
> viewmodel_bind
>
> ### Input (JSON schema)
> ```json
> {
>   "type": "object",
>   "properties": {
>     "path": {"type": "string", "description": "Widget Blueprint path, e.g. /Game/UI/WBP_Shop."},
>     "viewModel": {"type": "string", "description": "View model class (an MVVMViewModelBase subclass), e.g. /Script/MyGame.ShopViewModel or a Blueprint class path."},
>     "viewModelName": {"type": "string", "description": "Name of the view model in the widget; default is the class name."},
>     "creation": {"type": "string", "enum": ["CreateInstance", "Manual", "GlobalCollection", "PropertyPath"], "description": "How the widget gets its view model instance; default CreateInstance."},
>     "bindings": {
>       "type": "array",
>       "description": "Bindings to add.",
>       "items": {
>         "type": "object",
>         "properties": {
>           "widget": {"type": "string", "description": "Widget name in the tree, e.g. Gold."},
>           "property": {"type": "string", "description": "Widget property to set, e.g. Text."},
>           "source": {"type": "string", "description": "View model property (FieldNotify), e.g. Gold."},
>           "mode": {"type": "string", "enum": ["OneWayToWidget", "OneWayToViewModel", "TwoWay", "OneTimeToWidget"], "description": "Default OneWayToWidget."},
>           "conversion": {"type": "string", "description": "Optional conversion function, e.g. /Script/Engine.KismetTextLibrary:Conv_IntToText."}
>         },
>         "required": ["widget", "property", "source"]
>       }
>     },
>     "dryRun": {"type": "boolean", "description": "Only check and report what would change; write nothing."}
>   },
>   "required": ["path", "viewModel", "bindings"]
> }
> ```
>
> ### Expected output
> ```json
> {
>   "ok": true,
>   "data": {
>     "path": "/Game/UI/WBP_Shop",
>     "viewModel": {"name": "ShopViewModel", "class": "/Script/MyGame.ShopViewModel", "creation": "CreateInstance", "added": true},
>     "bindings": [
>       {"widget": "Gold", "property": "Text", "source": "Gold", "mode": "OneWayToWidget", "conversion": "Conv_IntToText", "added": true}
>     ],
>     "compiled": true,
>     "saved": true,
>     "readBack": {"viewModels": ["ShopViewModel"], "bindingCount": 1}
>   }
> }
> ```
> Errors: ASSET_NOT_FOUND (path), CLASS_NOT_FOUND (viewModel is not a view model class), WIDGET_NOT_FOUND, PROPERTY_NOT_FOUND (widget or view model property), TYPE_MISMATCH (needs a conversion function), PLUGIN_DISABLED (ModelViewViewModel plugin is off), INVALID_ARGUMENT. On any error nothing is written.
>
> ### Does it change the project?
> Writes assets or the level
>
> ### Can the change be undone?
> No (creates, renames, deletes or saves assets)
>
> ### Unreal Engine version
> 5.8
>
> ### Example in a project
> Blank project with the ModelViewViewModel plugin enabled. A view model class ShopViewModel (MVVMViewModelBase) with an int FieldNotify property Gold. A Widget Blueprint /Game/UI/WBP_Shop with a TextBlock named Gold. The tool adds ShopViewModel to WBP_Shop and binds Gold.Text to ShopViewModel.Gold through Conv_IntToText.
>
> ### Acceptance test
> 1. widget_create /Game/_EWTest/WBP_Shop, widget_add a TextBlock named Gold -> ok.
> 2. viewmodel_bind {path, viewModel: ShopViewModel, bindings: [{widget: "Gold", property: "Text", source: "Gold", conversion: "Conv_IntToText"}], dryRun: true} -> ok, reports one view model and one binding to add; widget_tree and the asset are unchanged.
> 3. Same call without dryRun -> ok, readBack lists ShopViewModel and one binding, compiled true.
> 4. Same call again -> ok, added false (no duplicate binding).
> 5. Same call with widget "Nope" -> WIDGET_NOT_FOUND, nothing written (binding count still 1).
> 6. Delete /Game/_EWTest/WBP_Shop.
>
> ### Anything else
> Plugin ModelViewViewModel (UMG Viewmodel). Classes I know are involved: UMVVMViewModelBase, UMVVMWidgetBlueprintExtension_View, UMVVMBlueprintView (AddViewModel, AddBinding), FMVVMBlueprintViewBinding. A read tool (viewmodel_list: view models and bindings of a widget) would help to check the result.

</details>

Every `###` heading matches a field of the [Tool request](https://github.com/shanyup/enginewright/issues/new?template=feature_request.yml) form, in the same order; the two dropdown answers are options of the form, and both JSON blocks parse. This is the kind of request we can turn into a tool: it names the task, shows that no existing tool covers it, and gives a test that can go into `tests/`.

## What happens next

The maintainer labels the request (`request`, then `accepted` or `needs-info`), writes the tool with tests, checks it on a test project and in a real game project, and ships it in a release. This is an early preview with one maintainer, so there is no promised date. Pull requests that follow [CONTRIBUTING.md](../CONTRIBUTING.md) are welcome.
