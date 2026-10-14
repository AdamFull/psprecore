# Cross-checks the psprecore::mips decoder against the PSP SDK's binutils (psp-as / psp-objdump).
#
#   1. tests/allegrex/all_ops.S (every instruction form) is assembled with psp-as and disassembled by both tools.
#   2. BOOT.BIN's .text (≈423k instructions) is disassembled by both tools, if the game is present.
#
# Requires a debug build (scripts\build.ps1) and the local "PSP SDK\pspsdk-MINPSPW" folder.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $root 'PSP SDK\pspsdk-MINPSPW\bin'
$psprx = Join-Path $root 'build\windows-x64-debug\tools\psprx\psprx.exe'
$compare = Join-Path $root 'tools\analysis\compare_objdump.py'
$out = Join-Path $root 'build\allegrex_check'
New-Item -ItemType Directory -Force $out | Out-Null

& "$sdk\psp-as.exe" -march=allegrex -o "$out\all_ops.o" (Join-Path $root 'tests\allegrex\all_ops.S')
if ($LASTEXITCODE) { throw 'psp-as failed' }
& "$sdk\psp-objdump.exe" -d "$out\all_ops.o" > "$out\all_ops_ref.dis"
& $psprx disasm "$out\all_ops.o" > "$out\all_ops_ours.dis"
python -I $compare "$out\all_ops_ref.dis" "$out\all_ops_ours.dis"
$failed = $LASTEXITCODE

$boot = Join-Path $root 'Crisis Core - Final Fantasy VII EU\PSP_GAME\SYSDIR\BOOT.BIN'
if (Test-Path $boot) {
    & "$sdk\psp-objdump.exe" -d -j .text $boot > "$out\boot_ref.dis"
    & $psprx disasm $boot > "$out\boot_ours.dis"
    python -I $compare "$out\boot_ref.dis" "$out\boot_ours.dis"
    if ($LASTEXITCODE) { $failed = 1 }
}
if ($failed) { throw 'decoder output differs from psp-objdump' }
