#Requires -Version 7
<#
.SYNOPSIS
    Configures (when needed), builds, and tests the Debug build in build/, then
    formats the C++ files changed on this branch.

.DESCRIPTION
    1. Configures with the given configure preset if build/CMakeCache.txt is
       missing.
    2. cmake --build --preset debug
    3. ctest --preset debug-cpu (CPU-only tests), or ctest --preset debug
       (every test) with -Full.
    4. clang-format -i on the *.cpp and *.hpp files under src/ and tests/ that
       differ from master (committed, uncommitted, or untracked), unless
       -NoFormat. clang-format is taken from PATH, else from the newest Visual
       Studio's bundled LLVM (found with vswhere).
    Stops at the first failing step with a nonzero exit code.

.EXAMPLE
    $env:ZENITH_SHARED_DEPS_DIR = 'F:/zenith/build/_deps'
    tools/check.ps1 -Preset worktree
#>
[CmdletBinding()]
param(
    # Run every test (Vulkan, benchmark, and slow ones too).
    [switch]$Full,
    # Skip formatting.
    [switch]$NoFormat,
    # Configure preset used when build/ is not configured yet.
    [string]$Preset = 'default'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot

function Invoke-Step([string]$description, [scriptblock]$command) {
    Write-Host "==> $description" -ForegroundColor Cyan
    & $command
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $description (exit code $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

function Find-ClangFormat {
    $onPath = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -prerelease -find '**\Llvm\x64\bin\clang-format.exe' | Select-Object -First 1
        if ($found) {
            return $found
        }
    }
    return $null
}

function Get-ChangedCppFiles {
    $names = @()
    git rev-parse --verify --quiet master | Out-Null
    if ($LASTEXITCODE -eq 0) {
        $names += git diff --name-only master...HEAD
    }
    $names += git diff --name-only HEAD
    $names += git ls-files --others --exclude-standard
    $global:LASTEXITCODE = 0
    return $names |
        Where-Object { $_ -match '^(src|tests)/.*\.(cpp|hpp)$' } |
        Sort-Object -Unique |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
}

Push-Location $repositoryRoot
try {
    if (-not (Test-Path 'build/CMakeCache.txt')) {
        Invoke-Step "Configure (preset $Preset)" { cmake --preset $Preset }
    }
    Invoke-Step 'Build (Debug)' { cmake --build --preset debug }
    $testPreset = if ($Full) { 'debug' } else { 'debug-cpu' }
    Invoke-Step "Test (preset $testPreset)" { ctest --preset $testPreset }

    if (-not $NoFormat) {
        $files = @(Get-ChangedCppFiles)
        if ($files.Count -eq 0) {
            Write-Host '==> Format: no changed C++ files' -ForegroundColor Cyan
        } else {
            $clangFormat = Find-ClangFormat
            if (-not $clangFormat) {
                Write-Host 'FAILED: clang-format not found on PATH or in Visual Studio (vswhere)' -ForegroundColor Red
                exit 1
            }
            Invoke-Step "Format $($files.Count) changed C++ file(s)" { & $clangFormat -i --style=file @files }
        }
    }
    Write-Host 'All checks passed.' -ForegroundColor Green
} finally {
    Pop-Location
}
