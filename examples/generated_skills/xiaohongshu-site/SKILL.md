---
name: xiaohongshu-site
description: "Use when Codex needs to work with Xiaohongshu through OpenCLI: search notes, inspect signed note URLs, read comments, inspect user profiles, download note media, review creator notes or analytics, or build direct Xiaohongshu URLs."
---

# Xiaohongshu

## Quick Start

Prefer registered AgentOS CLI skills when available:

```powershell
build\agentos.exe run xiaohongshu_search query="camping gear"
build\agentos.exe run xiaohongshu_note url="https://www.xiaohongshu.com/search_result/<id>?xsec_token=..."
build\agentos.exe run xiaohongshu_comments url="https://www.xiaohongshu.com/search_result/<id>?xsec_token=..."
build\agentos.exe run xiaohongshu_user profile="https://www.xiaohongshu.com/user/profile/<user-id>"
build\agentos.exe run xiaohongshu_creator_stats
```

For search-only URL resolution, use the shared `opencli` helper:

```powershell
build\agentos.exe run opencli site=xiaohongshu query="city walk"
```

Use direct URLs when the CLI is unavailable:

- Home: `https://www.xiaohongshu.com/`
- Search: `https://www.xiaohongshu.com/search_result?keyword=<url-encoded-query>`
- User profile: `https://www.xiaohongshu.com/user/profile/<user-id>`
- Note URLs: preserve the complete URL, especially `xsec_token` query parameters.

## Workflow

1. For discovery, call `xiaohongshu_search` or resolve a search URL with `opencli site=xiaohongshu`.
2. For note reading or comments, require a full signed note URL returned by search. Do not reconstruct or invent `xsec_token`.
3. For profile work, use `xiaohongshu_user` with the full profile URL when possible.
4. For creator analytics, use the creator commands only when the user is signed in to the relevant account.
5. For downloads, use `xiaohongshu_download` only for user-requested media extraction and keep output inside the workspace.

## OpenCLI Requirements

The browser-backed Xiaohongshu OpenCLI adapter requires Chrome to be running, logged in to `xiaohongshu.com`, and connected through the Browser Bridge extension. If OpenCLI is not installed, the local wrapper can still return a Xiaohongshu search URL for search tasks.

Read `references/opencli-commands.md` when a task needs the command catalog, argument details, or fallback behavior.

## Output Guidance

Summarize retrieved notes with title, author, URL, and engagement metrics when available. Keep signed URLs intact in follow-up commands. When access fails, report the likely missing prerequisite instead of retrying with guessed IDs or tokens.
