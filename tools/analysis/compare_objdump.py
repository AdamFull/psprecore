# Compares `psprx disasm` output with `psp-objdump -d` output line by line (same section, same addresses).
#
#   python -I compare_objdump.py <objdump.txt> <psprx.txt> [examples per mnemonic]
import collections
import re
import sys

pattern = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$')


def load(path):
    out = {}
    for line in open(path, encoding='utf-8-sig', errors='replace'):  # utf-8-sig: tolerate PowerShell's BOM
        m = pattern.match(line)
        if m:
            text = re.sub(r'([0-9a-f]+) <[^>]+>', lambda t: hex(int(t.group(1), 16)), m.group(3))  # '0 <sym>' -> 0x0
            out[int(m.group(1), 16)] = (m.group(2), ' '.join(text.split()))
    return out


ref, ours = load(sys.argv[1]), load(sys.argv[2])
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 3
bad = collections.defaultdict(list)
for addr, (word, text) in ref.items():
    mine = ours.get(addr, ('', '<missing>'))[1]
    if mine != text:
        bad[text.split(' ')[0]].append((addr, word, text, mine))
total = sum(len(v) for v in bad.values())
print(f"{len(ref)} instructions, {total} mismatches")
for mnemonic, rows in sorted(bad.items(), key=lambda kv: -len(kv[1])):
    print(f"  {mnemonic}: {len(rows)}")
    for addr, word, text, mine in rows[:limit]:
        print(f"      {addr:8x} {word}  objdump: {text:40s} ours: {mine}")
sys.exit(1 if total else 0)
