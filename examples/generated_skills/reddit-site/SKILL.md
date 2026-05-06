---
name: reddit-site
description: Use when Codex needs to open Reddit communities, posts, user profiles, or search Reddit for user discussions through direct URLs or opencli.
---

# Reddit

Use `opencli` when available to resolve Reddit search URLs:

```powershell
build\agentos.exe run opencli site=reddit query="agentos"
```

For direct URL construction, use:

- Home: `https://www.reddit.com/`
- Search: `https://www.reddit.com/search/?q=<url-encoded-query>`
- Subreddit: `https://www.reddit.com/r/<subreddit>/`
- User: `https://www.reddit.com/user/<username>/`

When looking for community discussion, include the product or topic name and any exact error text the user gave.
