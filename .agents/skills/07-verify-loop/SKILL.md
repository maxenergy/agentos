# verify-loop

## Purpose

Ensure Codex output is correct before proceeding.

## Steps

After each task:

1. Run verification command
2. Check test results
3. Check lint / type errors
4. Inspect git diff

## Output

- `docs/goal/VERIFY.md`

## Failure Handling

If verification fails:

- Capture logs
- Capture diff
- Send failure back to Codex
- Retry in same thread

## Rules

- Never proceed on failure
- Never skip verification
- Always include evidence
