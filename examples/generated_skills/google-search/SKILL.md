---
name: google-search
description: Use when Codex needs to prepare Google search URLs, search the public web from a user query, or route a general website lookup through the local opencli helper.
---

# Google Search

Use `opencli` when available to resolve Google URLs:

```powershell
build\agentos.exe run opencli site=google query="agentos cli skills"
```

For direct URL construction, use:

- Home: `https://www.google.com/`
- Search: `https://www.google.com/search?q=<url-encoded-query>`

Keep queries concise. Prefer adding domain filters such as `site:github.com` or `site:docs.example.com` when the user asks for a source-specific lookup.
