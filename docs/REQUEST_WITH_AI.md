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

Task given to the agent: *"Bind a TextBlock in my shop widget to a property of an MVVM view model."*

The agent called `doctor`, then `search` with "view model", "MVVM binding", "widget binding" and "bind widget property". The closest tools were `widget_set`, `widget_get` and `object_set`; their `describe` output has no binding arguments, so it wrote a request. Shortened:

> ### What should the agent be able to do?
> As a developer who uses UMG with the MVVM plugin, I want my agent to add a view model to a Widget Blueprint and bind a widget property to a view model property, so that it can wire a screen without me opening the View Bindings panel.
>
> ### How is it done today?
> By hand in the Widget Blueprint editor (Window > View Bindings). Tried `search` "view model", "MVVM binding", "widget binding": results were `widget_set`, `widget_get`, `object_set`; none of them can add a view model or a binding.
>
> ### Proposed tool name
> `viewmodel_bind`
>
> ### Input (JSON schema)
> ```json
> {"type":"object","properties":{
>   "path":{"type":"string","description":"Widget Blueprint path, e.g. /Game/UI/WBP_Shop"},
>   "viewModel":{"type":"string","description":"View model class (MVVMViewModelBase subclass)"},
>   "bindings":{"type":"array","description":"[{widget, property, source}] e.g. {\"widget\":\"Gold\",\"property\":\"Text\",\"source\":\"Gold\"}"},
>   "dryRun":{"type":"boolean","description":"Only report what would change"}},
>  "required":["path","viewModel","bindings"]}
> ```
> ...
>
> ### Acceptance test
> 1. `widget_create` /Game/_EWTest/WBP_Shop with a TextBlock "Gold"; a view model class with an int property Gold.
> 2. `viewmodel_bind` {path, viewModel, bindings:[{widget:"Gold", property:"Text", source:"Gold"}]} -> ok, the binding is read back, the Blueprint compiles.
> 3. Same call with widget "Nope" -> WIDGET_NOT_FOUND, nothing written.

This is the kind of request we can turn into a tool: it names the task, shows that no existing tool covers it, and gives a test that can go into `tests/`.

## What happens next

The maintainer labels the request (`request`, then `accepted` or `needs-info`), writes the tool with tests, checks it on a test project and in a real game project, and ships it in a release. This is an early preview with one maintainer, so there is no promised date. Pull requests that follow [CONTRIBUTING.md](../CONTRIBUTING.md) are welcome.
