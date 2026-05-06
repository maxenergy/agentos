---
name: wikipedia-site
description: Use when Codex needs to open Wikipedia articles, prepare encyclopedia lookup URLs, or search Wikipedia for a topic through opencli or direct URLs.
---

# Wikipedia

Use `opencli` when available to resolve Wikipedia search URLs:

```powershell
build\agentos.exe run opencli site=wikipedia query="large language model"
```

For direct URL construction, use:

- Portal: `https://www.wikipedia.org/`
- English article: `https://en.wikipedia.org/wiki/<title-with-underscores>`
- English search: `https://en.wikipedia.org/w/index.php?search=<url-encoded-query>`

For article URLs, replace spaces with underscores and preserve capitalization only when it is part of a proper noun or acronym.
