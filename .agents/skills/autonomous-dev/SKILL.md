---
name: autonomous-dev
description: Run a long autonomous development loop with planning, implementation, testing, documentation updates, and concise progress reports.
---

# Autonomous Development Loop

Use this skill when the user asks Codex to continue development for a long time with minimal supervision, such as "keep developing", "continue according to the roadmap", "work through the issues by priority", or "finish as much as possible without asking me".

This skill is a workflow. Project instructions in `AGENTS.md`, `CLAUDE.md`, `README.md`, and repository docs remain the source of truth for architecture, coding style, build commands, and safety constraints.

## Operating Rules

- Act autonomously, but stay conservative.
- Prefer small PR-sized batches that can be implemented, tested, and explained independently.
- Never perform unrelated refactors.
- Never rewrite architecture unless the current docs or code make that the explicit next task.
- Never use destructive git commands or revert user changes unless the user explicitly asks.
- Do not commit, push, open PRs, or change branches unless explicitly requested.
- Treat a dirty worktree as normal. Preserve unrelated user changes.
- Favor existing code patterns, helpers, storage formats, CLI conventions, and tests.
- Update docs whenever behavior, commands, capability status, or known gaps change.
- Keep progress updates brief and actionable during long work.

## Initial Orientation

Before choosing work, inspect the project enough to understand the current state:

- `AGENTS.md` if present
- `CLAUDE.md` or other local agent instructions if present
- `README.md`
- `plan.md`
- `completion_review.md` if present
- relevant files under `docs/`
- package/build files such as `CMakeLists.txt`, `package.json`, `pyproject.toml`, or equivalent
- existing tests for the area being changed
- `git status --short`

If there are issue lists, TODO sections, roadmap phases, or completion reviews, use their priority order unless the code clearly shows a safer or higher-impact prerequisite.

## Planning

Create a short plan before editing:

1. State the next target.
2. Explain why it is the next safe/high-value task.
3. Identify the files or modules likely to change.
4. Identify the tests or validation commands that should prove the batch.

The plan should be short. Do not stop after planning unless the user explicitly asked only for a plan.

## Batch Loop

Repeat this loop until the current user request is genuinely handled or a real blocker is reached:

1. Pick one bounded task from the roadmap, review, TODO list, failing test, or obvious implementation gap.
2. Read the relevant code and tests.
3. Implement the smallest coherent change.
4. Add or update focused tests when behavior changes.
5. Update docs and progress logs when commands, capabilities, gaps, or completion state change.
6. Run the narrowest relevant test first.
7. Fix failures.
8. Run broader verification before finishing.
9. Report what changed and what remains.

Prefer finishing one complete slice over starting several incomplete slices.

## Task Selection Heuristics

Choose work in this order:

1. Broken build, failing tests, or CI failures.
2. Review findings marked correct and actionable.
3. Incomplete roadmap items with clear local scope.
4. Missing tests for recently added behavior.
5. Documentation drift that could mislead future development.
6. Small admin/diagnostic UX that reduces future debugging cost.
7. Larger refactors only after the related behavior is stable and tested.

Avoid large dependency migrations, storage backend rewrites, broad API redesigns, or multi-module refactors unless they are the explicit task or the only practical blocker.

## Verification

Run the best available checks for the changed area.

For AgentOS C++ work, prefer:

```powershell
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"" -arch=x64 && cmake --build build"
ctest --test-dir build -R <relevant_test_name> --output-on-failure --timeout 60
ctest --test-dir build --output-on-failure --timeout 60
```

For other repositories, infer equivalent commands from README, AGENTS, package files, and CI configuration:

- lint if available
- unit tests if available
- integration tests relevant to the change
- build if available

If a long-running command hangs, stop the stale process if needed, rerun with an explicit timeout, and report that clearly.

## Blockers

If blocked:

- Write down the concrete blocker.
- Include the command, error, missing dependency, or ambiguous requirement.
- Try one safe workaround.
- Continue with independent safe work only if it does not hide the blocker.
- Ask the user only when the next step requires a product decision, secret, account access, destructive action, or substantial scope change.

## Progress Reports

During long autonomous work, report briefly after meaningful milestones:

- what context was gathered
- what change is being made
- what test is running
- what failed and how it is being fixed
- what remains after the current batch

Avoid noisy narration. Keep reports short and factual.

## Final Report

At the end of the run, return:

1. What was changed
2. Files modified
3. Tests run
4. Remaining risks or gaps
5. Recommended next task

If tests could not be run, say exactly why and name the command that should be run next.

