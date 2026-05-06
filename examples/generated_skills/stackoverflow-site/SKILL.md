---
name: stackoverflow-site
description: Use when Codex needs to search Stack Overflow questions, tags, answers, or programming errors through direct URLs or the opencli helper.
---

# Stack Overflow

Use `opencli` when available to resolve Stack Overflow search URLs:

```powershell
build\agentos.exe run opencli site=stackoverflow query="cmake windows ninja error"
```

For direct URL construction, use:

- Home: `https://stackoverflow.com/`
- Search: `https://stackoverflow.com/search?q=<url-encoded-query>`
- Tag: `https://stackoverflow.com/questions/tagged/<tag>`
- Question: `https://stackoverflow.com/questions/<question-id>`

For programming errors, include the exact error token and the language, framework, or tool name.
