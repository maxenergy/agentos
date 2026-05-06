---
name: youtube-site
description: Use when Codex needs to build YouTube video, channel, playlist, or search URLs from user input, or route YouTube lookup requests through opencli.
---

# YouTube

Use `opencli` when available to resolve YouTube search URLs:

```powershell
build\agentos.exe run opencli site=youtube query="agentos tutorial"
```

For direct URL construction, use:

- Home: `https://www.youtube.com/`
- Search: `https://www.youtube.com/results?search_query=<url-encoded-query>`
- Video: `https://www.youtube.com/watch?v=<video-id>`
- Playlist: `https://www.youtube.com/playlist?list=<playlist-id>`
- Channel handle: `https://www.youtube.com/@<handle>`

Preserve exact video IDs, playlist IDs, and handles when provided by the user.
