# Cross-checks the psprecore::mips decoder against the PSP SDK's binutils (psp-as / psp-objdump).
#
#   1. tests/allegrex/all_ops.S (every instruction form) is assembled with psp-as and disassembled by both tools.
#   2. A game's BOOT.BIN .text (-Boot) is disassembled by both tools, if given.
#
#   scripts\check_allegrex.ps1 [-Sdk <pspsdk folder>] [-Psprx <psprx.exe>] [-Boot <BOOT.BIN>]
#
# Needs a debug build of psprx (scripts\build.ps1) and a PSP SDK with binutils (default: "PSP SDK\pspsdk-MINPSPW"
# in this repository's root; it is never part of the repository).
param(
    [string]$Sdk,
    [string]$Psprx,
    [string]$Boot
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Sdk) { $Sdk = Join-Path $root 'PSP SDK\pspsdk-MINPSPW' }
$bin = Join-Path $Sdk 'bin'
if (-not $Psprx) {
    # Built on its own, or as a game's psprecore/ subdirectory.
    $Psprx = @((Join-Path $root 'build\windows-x64-debug\tools\psprx\psprx.exe'),
               (Join-Path (Split-Path -Parent $root) 'build\windows-x64-debug\psprecore\tools\psprx\psprx.exe')) |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Psprx) { throw 'psprx.exe not found: build the windows-x64-debug preset first or pass -Psprx' }
}
$compare = Join-Path $root 'tools\analysis\compare_objdump.py'
$out = Join-Path $root 'build\allegrex_check'
New-Item -ItemType Directory -Force $out | Out-Null

& "$bin\psp-as.exe" -march=allegrex -o "$out\all_ops.o" (Join-Path $root 'tests\allegrex\all_ops.S')
if ($LASTEXITCODE) { throw 'psp-as failed' }
& "$bin\psp-objdump.exe" -d "$out\all_ops.o" > "$out\all_ops_ref.dis"
& $Psprx disasm "$out\all_ops.o" > "$out\all_ops_ours.dis"
python -I $compare "$out\all_ops_ref.dis" "$out\all_ops_ours.dis"
$failed = $LASTEXITCODE

if ($Boot) {
    & "$bin\psp-objdump.exe" -d -j .text $Boot > "$out\boot_ref.dis"
    & $Psprx disasm $Boot > "$out\boot_ours.dis"
    python -I $compare "$out\boot_ref.dis" "$out\boot_ours.dis"
    if ($LASTEXITCODE) { $failed = 1 }
}
if ($failed) { throw 'decoder output differs from psp-objdump' }
