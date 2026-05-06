# Xiaohongshu OpenCLI Commands

Use this reference when a task needs the Xiaohongshu OpenCLI command catalog or direct command mapping.

Primary references: `https://opencli.info/docs/adapters/` and `https://opencli.info/docs/guide/browser-bridge.html`.

## AgentOS CLI Skills

- `xiaohongshu_search query="<keyword>"`: search notes by keyword. Falls back to a direct search URL if the external `opencli` binary is unavailable.
- `xiaohongshu_note url="<signed-note-url>"`: read one note. Requires a full signed note URL with `xsec_token`.
- `xiaohongshu_comments url="<signed-note-url>"`: read comments for one note.
- `xiaohongshu_user profile="<profile-url-or-id>"`: read public notes from a user profile.
- `xiaohongshu_download url="<signed-note-url-or-xhslink>"`: download note media into the workspace.
- `xiaohongshu_creator_notes`: list notes for the signed-in creator account.
- `xiaohongshu_creator_note_detail note="<note-id-or-url>"`: read detailed analytics for one creator note.
- `xiaohongshu_creator_stats`: read creator overview stats for the signed-in account.

## Native OpenCLI Mapping

These AgentOS skills call the native command shape below when `opencli` is installed:

```bash
opencli xiaohongshu search "<keyword>" --limit 10
opencli xiaohongshu note "<signed-note-url>"
opencli xiaohongshu comments "<signed-note-url>" --with-replies --limit 20
opencli xiaohongshu user "<profile-url-or-id>"
opencli xiaohongshu download "<signed-note-url-or-xhslink>"
opencli xiaohongshu creator-notes
opencli xiaohongshu creator-note-detail "<note-id-or-url>"
opencli xiaohongshu creator-notes-summary
opencli xiaohongshu creator-profile
opencli xiaohongshu creator-stats
```

The browser adapter also exposes feed and notification commands, but prefer explicit user consent before reading account-specific feeds or notifications.

## URL Rules

- Search URL: `https://www.xiaohongshu.com/search_result?keyword=<url-encoded-query>`
- Home URL: `https://www.xiaohongshu.com/`
- Note and comments commands need the full signed URL from search results. Bare note IDs are unreliable because tokenized URL parameters are required.
- Download accepts a full signed note URL or an `https://xhslink.com/...` short link.

## Access Requirements

OpenCLI's Xiaohongshu adapter is browser-backed. Chrome must be running, logged in to Xiaohongshu, and connected through Browser Bridge. If those prerequisites are missing, report the missing prerequisite and keep any generated URL or command for the user to run after login.
