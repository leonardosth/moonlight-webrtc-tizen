[CmdletBinding()]
param(
    [string]$TizenCli = 'C:\tizen-studio\tools\ide\bin\tizen.bat',
    [string]$SigningProfile,
    # Samsung's Emscripten SDK, required for the Wake-on-LAN module; see build-wake-on-lan.ps1.
    [string]$EmsdkRoot
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$outputDirectory = Join-Path $repositoryRoot 'dist\tizen'

if (-not (Test-Path -LiteralPath $TizenCli -PathType Leaf)) {
    throw "Tizen CLI was not found: $TizenCli"
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

function Build-TizenWidget {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$SourceDir,
        [Parameter(Mandatory=$true)][string]$ArtifactName,
        [string[]]$Files,
        [switch]$IncludeWasm
    )

    Write-Host "--- Building $Name ($ArtifactName) ---"
    $stageDir = Join-Path $outputDirectory '.source'
    $buildDir = Join-Path $outputDirectory '.build'
    $packageDir = Join-Path $outputDirectory '.package'
    $targetArtifact = Join-Path $outputDirectory $ArtifactName

    if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
    if (Test-Path -LiteralPath $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
    if (Test-Path -LiteralPath $packageDir) { Remove-Item -LiteralPath $packageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $stageDir, $packageDir -Force | Out-Null

    try {
        if ($Files -and $Files.Length -gt 0) {
            foreach ($file in $Files) {
                $filePath = Join-Path $SourceDir $file
                if (Test-Path -LiteralPath $filePath) {
                    Copy-Item -LiteralPath $filePath -Destination $stageDir
                }
            }
            if (Test-Path -LiteralPath (Join-Path $SourceDir 'assets')) {
                Copy-Item -LiteralPath (Join-Path $SourceDir 'assets') -Destination $stageDir -Recurse
            }
        } else {
            Copy-Item -Path "$SourceDir\*" -Destination $stageDir -Recurse
        }

        if ($IncludeWasm) {
            try {
                & (Join-Path $PSScriptRoot 'build-wake-on-lan.ps1') -OutputDirectory (Join-Path $stageDir 'wasm') -EmsdkRoot $EmsdkRoot
            } catch {
                Write-Warning "Wake-on-LAN WASM build skipped: $_"
                New-Item -ItemType Directory -Path (Join-Path $stageDir 'wasm') -Force | Out-Null
            }
        }

        & $TizenCli build-web --output $buildDir -- $stageDir
        if ($LASTEXITCODE -ne 0) {
            throw "Tizen web build failed for $Name with exit code $LASTEXITCODE."
        }
        Remove-Item -LiteralPath (Join-Path $buildDir 'tizen_web_project.yaml') -Force -ErrorAction SilentlyContinue

        $packageArguments = @('package', '-t', 'wgt', '--output', $packageDir)
        if (-not [string]::IsNullOrWhiteSpace($SigningProfile)) {
            $packageArguments += @('-s', $SigningProfile)
        }
        $packageArguments += @('--', $buildDir)
        & $TizenCli @packageArguments
        if ($LASTEXITCODE -ne 0) {
            throw "Tizen WGT package signing failed for $Name with exit code $LASTEXITCODE."
        }

        $packages = @(Get-ChildItem -LiteralPath $packageDir -Filter '*.wgt' -File)
        if ($packages.Count -ne 1) {
            throw "Expected one generated WGT for $Name, found $($packages.Count)."
        }
        if (Test-Path -LiteralPath $targetArtifact) {
            Remove-Item -LiteralPath $targetArtifact -Force
        }
        Move-Item -LiteralPath $packages[0].FullName -Destination $targetArtifact
        Write-Host "Tizen WGT created: $targetArtifact"
    } finally {
        Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $packageDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# 1. Build Moonlight WebRTC Client (Main app)
$moonlightRuntimeFiles = @(
    'app.js',
    'application-artwork.js',
    'config.xml',
    'durable-storage.js',
    'frame-interpolation.js',
    'gamepad-input.js',
    'gamepad-ui-navigation.js',
    'gateway-ipv4.js',
    'gateway-store.js',
    'index.html',
    'preferences.js',
    'tizen_web_project.yaml',
    'ui.css',
    'ui.js',
    'wake-on-lan.js'
)
Build-TizenWidget -Name 'Moonlight WebRTC Client' -SourceDir (Join-Path $repositoryRoot 'tizen') -ArtifactName 'MoonlightWebRTC.wgt' -Files $moonlightRuntimeFiles -IncludeWasm

# 2. Build Steam Big Picture (Shortcut launcher app)
Build-TizenWidget -Name 'Steam Big Picture' -SourceDir (Join-Path $repositoryRoot 'tizen-steam') -ArtifactName 'SteamBigPicture.wgt'
