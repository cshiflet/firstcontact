$ErrorActionPreference = "Stop"

function Test-Command {
    param([Parameter(Mandatory=$true)][string]$Name)
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Install-WingetPackage {
    param(
        [Parameter(Mandatory=$true)][string]$Id,
        [Parameter(Mandatory=$true)][string]$Name,
        [string]$Override = ""
    )
    if (-not (Test-Command winget)) {
        throw "winget is required. Install App Installer from Microsoft Store, then rerun this script."
    }
    $existing = winget list --id $Id --exact 2>$null
    if ($LASTEXITCODE -eq 0 -and ($existing -match [regex]::Escape($Id))) {
        Write-Host "✓ $Name already installed"
        return
    }
    Write-Host "→ installing $Name"
    if ([string]::IsNullOrWhiteSpace($Override)) {
        winget install --id $Id --exact --accept-package-agreements --accept-source-agreements
    } else {
        winget install --id $Id --exact --accept-package-agreements --accept-source-agreements --override $Override
    }
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ToolsRoot = Join-Path $env:USERPROFILE "firstcontact-tools"
$QtVersion = $env:FC_QT_VERSION
if ([string]::IsNullOrWhiteSpace($QtVersion)) { $QtVersion = "6.7.3" }
$QtArch = "win64_msvc2022_64"
$QtRoot = Join-Path $ToolsRoot "Qt"
$QtPrefix = Join-Path $QtRoot "$QtVersion\msvc2022_64"
$VcpkgRoot = $env:VCPKG_ROOT
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = Join-Path $ToolsRoot "vcpkg"
}

New-Item -ItemType Directory -Force -Path $ToolsRoot | Out-Null

Install-WingetPackage -Id "Git.Git" -Name "Git"
Install-WingetPackage -Id "Kitware.CMake" -Name "CMake"
Install-WingetPackage -Id "Ninja-build.Ninja" -Name "Ninja"
Install-WingetPackage -Id "Python.Python.3.12" -Name "Python 3"
Install-WingetPackage `
    -Id "Microsoft.VisualStudio.2022.BuildTools" `
    -Name "Visual Studio 2022 Build Tools" `
    -Override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

if (-not (Test-Command python)) {
    throw "python was installed but is not on PATH yet. Open a new Developer PowerShell and rerun this script."
}

python -m pip install --user --upgrade pip aqtinstall

if (Test-Path $QtPrefix) {
    Write-Host "✓ Qt $QtVersion MSVC 2022 already installed at $QtPrefix"
} else {
    Write-Host "→ installing Qt $QtVersion MSVC 2022 via aqtinstall"
    python -m aqt install-qt windows desktop $QtVersion $QtArch `
        --outputdir $QtRoot `
        --modules qttools qtnetworkauth qtsvg
}

if (Test-Path (Join-Path $VcpkgRoot ".git")) {
    Write-Host "✓ vcpkg already present at $VcpkgRoot"
    git -C $VcpkgRoot pull --ff-only
} else {
    Write-Host "→ cloning vcpkg to $VcpkgRoot"
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
}

$Bootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
& $Bootstrap | Out-Host

$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
& $VcpkgExe install --triplet x64-windows --x-manifest-root=$RepoRoot | Out-Host

$Toolchain = (Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake").Replace("\", "/")
$QtPrefixJson = $QtPrefix.Replace("\", "/")
$UserPresets = Join-Path $RepoRoot "CMakeUserPresets.json"

$Json = @"
{
    "version": 6,
    "configurePresets": [
        {
            "name": "windows-msvc-debug-local",
            "displayName": "Windows MSVC Debug (local Qt/vcpkg)",
            "inherits": "windows-msvc-debug",
            "cacheVariables": {
                "CMAKE_PREFIX_PATH": "$QtPrefixJson",
                "CMAKE_TOOLCHAIN_FILE": "$Toolchain",
                "VCPKG_TARGET_TRIPLET": "x64-windows"
            }
        },
        {
            "name": "windows-msvc-release-local",
            "displayName": "Windows MSVC Release (local Qt/vcpkg)",
            "inherits": "windows-msvc-release",
            "cacheVariables": {
                "CMAKE_PREFIX_PATH": "$QtPrefixJson",
                "CMAKE_TOOLCHAIN_FILE": "$Toolchain",
                "VCPKG_TARGET_TRIPLET": "x64-windows"
            }
        }
    ],
    "buildPresets": [
        { "name": "windows-msvc-debug-local", "configurePreset": "windows-msvc-debug-local" },
        { "name": "windows-msvc-release-local", "configurePreset": "windows-msvc-release-local" }
    ],
    "testPresets": [
        {
            "name": "windows-msvc-debug-local",
            "configurePreset": "windows-msvc-debug-local",
            "output": { "outputOnFailure": true }
        }
    ]
}
"@

Set-Content -Path $UserPresets -Value $Json -Encoding UTF8

Write-Host ""
Write-Host "Windows setup complete."
Write-Host "Use a 'Developer PowerShell for VS 2022' so cl.exe is on PATH."
Write-Host "Configure: cmake --preset windows-msvc-debug-local"
Write-Host "Build:     cmake --build --preset windows-msvc-debug-local"
Write-Host "Test:      ctest --preset windows-msvc-debug-local"
