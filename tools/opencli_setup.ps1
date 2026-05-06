param(
    [Parameter(Position = 0)]
    [ValidateSet("bootstrap", "doctor", "install", "extension")]
    [string]$Action = "bootstrap",

    [switch]$OpenBrowser
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding

function Write-Json($Object) {
    $Object | ConvertTo-Json -Compress -Depth 8
}

function Quote-ProcessArgument([string]$Argument) {
    if ($null -eq $Argument -or $Argument.Length -eq 0) {
        return '""'
    }
    if ($Argument -notmatch '[\s"]') {
        return $Argument
    }

    $quoted = '"'
    $backslashes = 0
    foreach ($char in $Argument.ToCharArray()) {
        if ($char -eq '\') {
            $backslashes++
        } elseif ($char -eq '"') {
            $quoted += ('\' * (($backslashes * 2) + 1))
            $quoted += '"'
            $backslashes = 0
        } else {
            if ($backslashes -gt 0) {
                $quoted += ('\' * $backslashes)
                $backslashes = 0
            }
            $quoted += $char
        }
    }
    if ($backslashes -gt 0) {
        $quoted += ('\' * ($backslashes * 2))
    }
    $quoted += '"'
    return $quoted
}

function Invoke-Native([string]$Command, [string[]]$Arguments, [int]$TimeoutSeconds = 60) {
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo.FileName = $Command
    $process.StartInfo.Arguments = (($Arguments | ForEach-Object { Quote-ProcessArgument ([string]$_) }) -join " ")
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.CreateNoWindow = $true

    try {
        [void]$process.Start()
        $completed = $process.WaitForExit($TimeoutSeconds * 1000)
        if (-not $completed) {
            try {
                $process.Kill($true)
            } catch {
                try { $process.Kill() } catch { }
            }
        }
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        $exitCode = if ($completed) { [int]$process.ExitCode } else { 124 }
        return @{
            command = "$Command $($Arguments -join ' ')"
            exit_code = $exitCode
            stdout = (($stdout, $stderr | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join "`n")
            stderr = $stderr
            timed_out = (-not $completed)
        }
    } finally {
        $process.Dispose()
    }
}

function Resolve-OpenCliRunner {
    $opencli = Get-Command opencli -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $opencli) {
        return @{
            command = $opencli.Source
            prefix = @()
            display = "opencli"
            source = "path"
        }
    }

    $localCmd = Join-Path (Get-Location) "runtime\tools\opencli\node_modules\.bin\opencli.cmd"
    if (Test-Path -LiteralPath $localCmd) {
        return @{
            command = $localCmd
            prefix = @()
            display = $localCmd
            source = "workspace"
        }
    }

    $localPs1 = Join-Path (Get-Location) "runtime\tools\opencli\node_modules\.bin\opencli.ps1"
    if (Test-Path -LiteralPath $localPs1) {
        return @{
            command = "powershell"
            prefix = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $localPs1)
            display = $localPs1
            source = "workspace"
        }
    }

    $npx = Get-Command npx -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $npx) {
        return @{
            command = $npx.Source
            prefix = @("-y", "@jackwener/opencli")
            display = "npx -y @jackwener/opencli"
            source = "npx"
        }
    }

    return $null
}

function Install-WorkspaceOpenCli {
    $npm = Get-Command npm -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $npm) {
        return @{
            ok = $false
            error = "npm_not_found"
            message = "Node.js/npm is required to install @jackwener/opencli."
        }
    }

    $installDir = Join-Path (Get-Location) "runtime\tools\opencli"
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
    $result = Invoke-Native $npm.Source @("install", "--prefix", $installDir, "@jackwener/opencli") 180
    return @{
        ok = ($result.exit_code -eq 0)
        install_dir = $installDir
        result = $result
    }
}

function Install-ExtensionBundle {
    $targetRoot = Join-Path (Get-Location) "runtime\tools\opencli"
    $extensionDir = Join-Path $targetRoot "browser-bridge-extension"
    $assetPath = Join-Path $targetRoot "opencli-extension.zip"
    New-Item -ItemType Directory -Force -Path $targetRoot | Out-Null

    $existingManifest = $null
    if (Test-Path -LiteralPath $extensionDir) {
        $existingManifest = Get-ChildItem -LiteralPath $extensionDir -Filter manifest.json -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    }

    $releaseTag = $null
    $assetName = $null
    if ($null -eq $existingManifest) {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/jackwener/opencli/releases/latest" `
            -Headers @{ "User-Agent" = "agentos-opencli-setup" }
        $asset = $release.assets | Where-Object { $_.name -like "*extension*.zip" } | Select-Object -First 1
        if ($null -eq $asset) {
            return @{
                ok = $false
                error = "extension_asset_not_found"
                release = $release.tag_name
            }
        }

        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $assetPath -Headers @{ "User-Agent" = "agentos-opencli-setup" }
        if (Test-Path -LiteralPath $extensionDir) {
            Remove-Item -LiteralPath $extensionDir -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $extensionDir | Out-Null
        Expand-Archive -LiteralPath $assetPath -DestinationPath $extensionDir -Force
        $releaseTag = $release.tag_name
        $assetName = $asset.name
    }

    $manifest = Get-ChildItem -LiteralPath $extensionDir -Filter manifest.json -Recurse | Select-Object -First 1
    $loadPath = if ($null -ne $manifest) { Split-Path -Parent $manifest.FullName } else { $extensionDir }

    $browserLaunch = $null
    if ($OpenBrowser) {
        $sessionId = [System.DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        $profileDir = Join-Path $targetRoot ("browser-profile\session-" + $sessionId)
        New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
        $pathCopiedToClipboard = $false
        try {
            Set-Clipboard -Value $loadPath
            $pathCopiedToClipboard = $true
        } catch {
            $pathCopiedToClipboard = $false
        }
        try {
            Start-Process -FilePath "explorer.exe" -ArgumentList @($loadPath)
        } catch {
        }
        $browserCandidates = @(
            "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
            "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
            "$env:LocalAppData\Google\Chrome\Application\chrome.exe",
            "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
            "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
        ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_) }
        $browser = $browserCandidates | Select-Object -First 1
        if ($null -ne $browser) {
            $browserArgs = @(
                "--user-data-dir=$profileDir",
                "--load-extension=$loadPath",
                "--disable-extensions-except=$loadPath",
                "--no-first-run",
                "--no-default-browser-check",
                "https://opencli.info/docs/guide/browser-bridge.html",
                "chrome://extensions/"
            )
            Start-Process -FilePath $browser -ArgumentList $browserArgs
            $browserLaunch = @{
                ok = $true
                browser = $browser
                load_extension = $loadPath
                user_data_dir = $profileDir
                arguments = $browserArgs
                extension_dir_opened = $true
                extension_dir_copied_to_clipboard = $pathCopiedToClipboard
            }
        } else {
            $browserLaunch = @{
                ok = $false
                error = "browser_not_found"
                extension_dir_opened = $true
                extension_dir_copied_to_clipboard = $pathCopiedToClipboard
            }
        }
    }

    return @{
        ok = $true
        release = $releaseTag
        asset = $assetName
        zip = $assetPath
        extension_dir = $loadPath
        browser_launch = $browserLaunch
    }
}

function Invoke-OpenCliDoctor($Runner) {
    if ($null -eq $Runner) {
        return @{
            ok = $false
            error = "opencli_runner_not_found"
        }
    }
    $args = @($Runner.prefix) + @("doctor")
    $result = Invoke-Native $Runner.command $args 15
    $text = $result.stdout
    $bridgeConnected = ($text -match "\[OK\]\s+Connectivity" -or $text -match "Connectivity:\s+ok")
    return @{
        ok = (($result.exit_code -eq 0) -and $bridgeConnected)
        command_ok = ($result.exit_code -eq 0)
        result = $result
        daemon_ok = ($text -match "\[OK\]\s+Daemon")
        extension_connected = ($text -match "\[OK\]\s+Extension" -or $text -match "Extension:\s+connected")
        browser_bridge_connected = $bridgeConnected
        browser_bridge_missing = ($text -match "Browser Bridge extension not connected" -or $text -match "\[MISSING\]\s+Extension")
    }
}

function Wait-OpenCliDoctor($Runner, [int]$Attempts = 5, [int]$DelaySeconds = 2) {
    $last = $null
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        $last = Invoke-OpenCliDoctor $Runner
        $last["attempt"] = $attempt
        $last["max_attempts"] = $Attempts
        if ($last.ok) {
            return $last
        }
        if ($attempt -lt $Attempts) {
            Start-Sleep -Seconds $DelaySeconds
        }
    }
    return $last
}

$install = $null
$extension = $null
$runner = Resolve-OpenCliRunner

if ($Action -in @("bootstrap", "install") -and ($null -eq $runner -or $runner.source -eq "npx")) {
    $install = Install-WorkspaceOpenCli
    $runner = Resolve-OpenCliRunner
}

if ($Action -in @("bootstrap", "extension")) {
    try {
        $extension = Install-ExtensionBundle
    } catch {
        $extension = @{
            ok = $false
            error = "extension_prepare_failed"
            message = $_.Exception.Message
        }
    }
}

$doctorAttempts = if ($OpenBrowser -and $Action -in @("bootstrap", "extension")) { 5 } else { 1 }
$doctor = if ($Action -ne "install") { Wait-OpenCliDoctor $runner $doctorAttempts } else { $null }
$ok = $true
if ($Action -eq "install") {
    $ok = ($null -ne $runner)
} elseif ($Action -eq "extension") {
    $ok = ($null -ne $extension -and $extension.ok -and $null -ne $doctor -and $doctor.ok)
} elseif ($null -eq $doctor -or -not $doctor.ok) {
    $ok = $false
}

$browserBridgeMissing = ($null -ne $doctor -and $doctor.browser_bridge_missing)
$humanActionRequired = (-not $ok -and $browserBridgeMissing)
$extensionDirForUser = if ($null -ne $extension -and $extension.extension_dir) { $extension.extension_dir } else { $null }

Write-Json @{
    ok = $ok
    action = $Action
    human_action_required = $humanActionRequired
    blocked_by = if ($humanActionRequired) { "browser_bridge_extension_not_connected" } else { $null }
    opencli_runner = $runner
    install = $install
    extension = $extension
    doctor = $doctor
    next_action = if ($humanActionRequired) {
        "Load the unpacked Browser Bridge extension in Chrome/Edge, keep that browser open, log in to the target website if needed, then rerun the original AgentOS request."
    } else {
        $null
    }
    manual_steps = @(
        "Extension directory: $extensionDirForUser",
        "The extension directory was opened in Explorer and copied to the clipboard when possible.",
        "Open chrome://extensions/ and enable Developer Mode.",
        "Click Load unpacked and select the extension directory reported in extension.extension_dir.",
        "Keep that browser window open so the Browser Bridge extension stays connected.",
        "Log in to the target website in that browser.",
        "Run: npx -y @jackwener/opencli doctor"
    )
}

if ($ok) {
    exit 0
}
exit 69
