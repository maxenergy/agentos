---
name: github-site
description: Use when Codex needs to open or search GitHub repositories, users, organizations, issues, pull requests, releases, discussions, or code search pages through URLs or opencli.
---

# GitHub

Use `opencli` when available to resolve GitHub search URLs:

```powershell
build\agentos.exe run opencli site=github query="owner repo issue text"
```

For direct URL construction, use:

- Home: `https://github.com/`
- Repository: `https://github.com/<owner>/<repo>`
- Issues: `https://github.com/<owner>/<repo>/issues`
- Pull requests: `https://github.com/<owner>/<repo>/pulls`
- Releases: `https://github.com/<owner>/<repo>/releases`
- Search: `https://github.com/search?q=<url-encoded-query>`

When the task is about a specific repo, prefer the canonical repo URL over a broad search URL.
