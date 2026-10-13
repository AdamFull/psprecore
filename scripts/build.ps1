# Configure, build and (optionally) test a Windows preset without opening a developer prompt.
#
#   scripts\build.ps1                       # windows-x64-debug, build + tests
#   scripts\build.ps1 -Preset windows-x64-release -NoTest
param(
    [string]$Preset = 'windows-x64-debug',
    [switch]$NoTest
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x64 tools was not found.' }

# Import the x64 developer environment (Windows SDK, CRT) into this PowerShell session.
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars itself calls vswhere
cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

# Prefer standalone LLVM and VS-bundled CMake/Ninja when they are not already on PATH.
$extra = @(
    "$env:ProgramFiles\LLVM\bin",
    "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
    "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
) | Where-Object { Test-Path $_ }
$env:PATH = ($extra + $env:PATH) -join ';'

Push-Location $root
try {
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
    if (-not $NoTest) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }
    }
} finally {
    Pop-Location
}
