<!-- Thanks for the pull request. Please read CONTRIBUTING.md first. Keep one topic per PR. -->

## What and why

<!-- What does this change do? Link the issue: "Closes #12". -->

## Tools added or changed

<!-- One line per tool: name, read or write, what changed. Write "none" for docs or test-only changes. -->

| Tool | Read / write | Change |
|---|---|---|
| | | |

## Tests

<!-- Which test file did you extend? Paste the summary of tests/run_all.py from a running editor on a test project. -->

```text
python tests/run_all.py
...
```

Unreal Engine version and platform used:

## Checklist

- [ ] `python tests/run_all.py` passes on a separate test project (paste above). If I changed build tools: `python tests/run_all.py build` too.
- [ ] New or changed tools have tests for the success case and for each error code they return.
- [ ] No leftovers: the tests delete what they create (`/Game/_EWTest`, actors in the level, files), and a second run passes.
- [ ] Every write is validated before anything changes and read back after; the reply shows the read-back values.
- [ ] Writes are one undo step (or the tool is registered as not transactional, with the reason in a comment) and are refused while Play In Editor runs.
- [ ] Every new argument has a `description` in the schema; unknown arguments and bad values return an error with a suggestion.
- [ ] README tool table and `docs/ROADMAP.md` are updated if the tool list changed.
- [ ] Builds without warnings in the plugin on UE 5.8 Win64.
