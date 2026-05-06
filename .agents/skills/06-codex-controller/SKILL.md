# codex-controller (v2)

## Purpose

Control Codex `/goal` execution reliably.

## Rules

- Use codex app-server
- Single persistent thread
- Execute one task at a time
- After each task: verify
- If fail: feed errors back into same thread
- Never restart thread unless explicitly required

## Loop

for task in TASKS.md:
  run task
  run verification
  if fail:
    send error to Codex
    retry

## Safety

- Check git diff
- Reject unexpected file changes
- Allow rollback
