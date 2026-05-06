param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet(
        "search",
        "note",
        "comments",
        "feed",
        "notifications",
        "user",
        "download",
        "creator-notes",
        "creator-note-detail",
        "creator-notes-summary",
        "creator-profile",
        "creator-stats",
        "search-url"
    )]
    [string]$Action,

    [Parameter(Position = 1)]
    [string]$Target = "",

    [Parameter(Position = 2)]
    [int]$Limit = 10,

    [Parameter(Position = 3)]
    [string]$Format = "",

    [Parameter(Position = 4)]
    [string]$Replies = "",

    [Parameter(Position = 5)]
    [int]$OpenCliTimeoutSeconds = 45
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding

function Write-Json($Object) {
    $Object | ConvertTo-Json -Compress -Depth 8
}

function Quote-NativeArgument([string]$Value) {
    if ($null -eq $Value) {
        return '""'
    }
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Get-SearchUrl([string]$Query) {
    $encodedQuery = [System.Uri]::EscapeDataString($Query)
    return "https://www.xiaohongshu.com/search_result?keyword=$encodedQuery"
}

function Require-Target([string]$Kind) {
    if ([string]::IsNullOrWhiteSpace($Target)) {
        Write-Json @{
            ok = $false
            action = $Action
            error = "missing_$Kind"
        }
        exit 2
    }
}

function Resolve-OpenCliRunner {
    $opencli = Get-Command opencli -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $opencli) {
        return @{
            Command = $opencli.Source
            Prefix = @()
            Display = "opencli"
        }
    }

    $localCmd = Join-Path (Get-Location) "runtime\tools\opencli\node_modules\.bin\opencli.cmd"
    if (Test-Path -LiteralPath $localCmd) {
        return @{
            Command = $localCmd
            Prefix = @()
            Display = $localCmd
        }
    }

    $localPs1 = Join-Path (Get-Location) "runtime\tools\opencli\node_modules\.bin\opencli.ps1"
    if (Test-Path -LiteralPath $localPs1) {
        return @{
            Command = "powershell"
            Prefix = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $localPs1)
            Display = $localPs1
        }
    }

    $npx = Get-Command npx -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $npx) {
        return @{
            Command = $npx.Source
            Prefix = @("-y", "@jackwener/opencli")
            Display = "npx -y @jackwener/opencli"
        }
    }

    return $null
}

function Invoke-OpenCli([string[]]$OpenCliArgs) {
    $fullArgs = @("xiaohongshu") + $OpenCliArgs
    $runner = Resolve-OpenCliRunner
    if ($null -eq $runner) {
        Write-Json @{
            ok = $false
            action = $Action
            error = "opencli_not_found"
            requirement = "Install OpenCLI with: npm install -g @jackwener/opencli"
        }
        exit 127
    }

    $invokeArgs = @($runner.Prefix) + $fullArgs
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("agentos-opencli-" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    $stdoutPath = Join-Path $tempRoot "stdout.txt"
    $stderrPath = Join-Path $tempRoot "stderr.txt"

    try {
        $argumentLine = ($invokeArgs | ForEach-Object { Quote-NativeArgument $_ }) -join " "
        $process = Start-Process `
            -FilePath $runner.Command `
            -ArgumentList $argumentLine `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -WindowStyle Hidden `
            -PassThru
    } catch {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
        Write-Json @{
            ok = $false
            action = $Action
            command = "$($runner.Display) $($fullArgs -join ' ')"
            stdout = ""
            exit_code = 126
            error = "opencli_start_failed"
            message = $_.Exception.Message
        }
        exit 126
    }

    $completed = $process.WaitForExit($OpenCliTimeoutSeconds * 1000)
    if (-not $completed) {
        if ($IsWindows -or $env:OS -eq "Windows_NT") {
            & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null
        } else {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Milliseconds 200
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
        Write-Json @{
            ok = $false
            action = $Action
            command = "$($runner.Display) $($fullArgs -join ' ')"
            stdout = ""
            exit_code = 124
            error = "opencli_timeout"
            timeout_seconds = $OpenCliTimeoutSeconds
            fallback_url = if ($Action -eq "search") { Get-SearchUrl $Target } else { $null }
            help = "OpenCLI did not return before the AgentOS tool timeout. Check Browser Bridge/login state with: opencli doctor"
        }
        exit 124
    }

    $exitCode = [int]$process.ExitCode
    $stdout = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue } else { "" }
    $stderr = if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath -Raw -ErrorAction SilentlyContinue } else { "" }
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    $combinedOutput = (($stdout, $stderr) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join "`n"
    if ($exitCode -eq 0 -and ($combinedOutput -match "ok:\s*false" -or $combinedOutput -match "BROWSER_CONNECT")) {
        $exitCode = 69
    }

    $browserBridgeHelp = if ($combinedOutput -match "BROWSER_CONNECT") {
        "Chrome may be logged in, but OpenCLI Browser Bridge is not connected. Keep Chrome open, enable the Browser Bridge extension, then run: opencli daemon stop && opencli doctor"
    } else {
        $null
    }

    Write-Json @{
        ok = ($exitCode -eq 0)
        action = $Action
        command = "$($runner.Display) $($fullArgs -join ' ')"
        stdout = $combinedOutput
        exit_code = $exitCode
        fallback_url = if ($Action -eq "search" -and $exitCode -ne 0) { Get-SearchUrl $Target } else { $null }
        help = $browserBridgeHelp
    }
    exit $exitCode
}

if ($Action -eq "search-url") {
    Require-Target "query"
    Write-Json @{
        ok = $true
        action = $Action
        query = $Target
        url = (Get-SearchUrl $Target)
    }
    exit 0
}

switch ($Action) {
    "search" {
        Require-Target "query"
        $args = @("search", $Target, "--limit", "$Limit")
        if (-not [string]::IsNullOrWhiteSpace($Format)) {
            $args += @("-f", $Format)
        }
        Invoke-OpenCli $args
    }
    "note" {
        Require-Target "url"
        $args = @("note", $Target)
        if (-not [string]::IsNullOrWhiteSpace($Format)) {
            $args += @("-f", $Format)
        }
        Invoke-OpenCli $args
    }
    "comments" {
        Require-Target "url"
        $effectiveLimit = if ($Limit -gt 0) { $Limit } else { 20 }
        $args = @("comments", $Target, "--limit", "$effectiveLimit")
        if ($Replies -in @("true", "1", "yes", "with-replies", "with_replies")) {
            $args += "--with-replies"
        }
        if (-not [string]::IsNullOrWhiteSpace($Format)) {
            $args += @("-f", $Format)
        }
        Invoke-OpenCli $args
    }
    "user" {
        Require-Target "profile"
        Invoke-OpenCli @("user", $Target)
    }
    "download" {
        Require-Target "url"
        Invoke-OpenCli @("download", $Target)
    }
    "creator-note-detail" {
        Require-Target "note"
        Invoke-OpenCli @("creator-note-detail", $Target)
    }
    default {
        Invoke-OpenCli @($Action)
    }
}
