param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("resolve", "open", "list")]
    [string]$Action,

    [string]$Site = "",
    [string]$Query = ""
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding

$sites = @{
    "google" = @{
        home = "https://www.google.com/"
        search = "https://www.google.com/search?q={0}"
    }
    "github" = @{
        home = "https://github.com/"
        search = "https://github.com/search?q={0}"
    }
    "youtube" = @{
        home = "https://www.youtube.com/"
        search = "https://www.youtube.com/results?search_query={0}"
    }
    "wikipedia" = @{
        home = "https://www.wikipedia.org/"
        search = "https://en.wikipedia.org/w/index.php?search={0}"
    }
    "stackoverflow" = @{
        home = "https://stackoverflow.com/"
        search = "https://stackoverflow.com/search?q={0}"
    }
    "reddit" = @{
        home = "https://www.reddit.com/"
        search = "https://www.reddit.com/search/?q={0}"
    }
    "xiaohongshu" = @{
        home = "https://www.xiaohongshu.com/"
        search = "https://www.xiaohongshu.com/search_result?keyword={0}"
    }
    "xhs" = @{
        home = "https://www.xiaohongshu.com/"
        search = "https://www.xiaohongshu.com/search_result?keyword={0}"
    }
    "rednote" = @{
        home = "https://www.xiaohongshu.com/"
        search = "https://www.xiaohongshu.com/search_result?keyword={0}"
    }
}

function Write-Json($Object) {
    $Object | ConvertTo-Json -Compress
}

if ($Action -eq "list") {
    Write-Json @{
        ok = $true
        sites = @($sites.Keys | Sort-Object)
    }
    exit 0
}

$normalizedSite = $Site.ToLowerInvariant()
if (-not $sites.ContainsKey($normalizedSite)) {
    Write-Json @{
        ok = $false
        error = "unknown_site"
        site = $Site
        supported_sites = @($sites.Keys | Sort-Object)
    }
    exit 2
}

$siteSpec = $sites[$normalizedSite]
$encodedQuery = [System.Uri]::EscapeDataString($Query)
$url = if ([string]::IsNullOrWhiteSpace($Query)) {
    $siteSpec.home
} else {
    [string]::Format($siteSpec.search, $encodedQuery)
}

if ($Action -eq "open") {
    Start-Process $url
}

Write-Json @{
    ok = $true
    action = $Action
    site = $normalizedSite
    query = $Query
    url = $url
}
