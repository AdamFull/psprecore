# Adds NID names missing from a NID database using a PPSSPP debug log (--log=<file> -d) of the game booting.
#
# PPSSPP logs every resolved import as "Importing <name> : <stub address>". Combined with `ccprx imports`
# (library, NID, module-relative stub address) this yields NID -> name for libraries the homebrew SDK lacks.
# Only names are taken from the log (observed output); no emulator code is involved.
#
#   ccprx imports BOOT.BIN nids.tsv > imports.tsv
#   python -I nids_from_ppsspp.py <ppsspp log> imports.tsv nids.tsv [load base, default 0x08804000]
import re
import sys

log_path, imports_path, db_path = sys.argv[1:4]
base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x08804000

by_stub = {}
for line in open(log_path, encoding='utf8', errors='replace'):
    m = re.search(r'Importing (\w+) : ([0-9a-f]{8})', line)
    if m:
        by_stub[int(m.group(2), 16)] = m.group(1)

known = set()
for line in open(db_path, encoding='utf8'):
    if not line.startswith('#'):
        library, nid, _ = line.rstrip('\n').split('\t')
        known.add((library, nid))

added = []
for line in open(imports_path, encoding='utf8'):
    fields = line.rstrip('\n').split('\t')
    if len(fields) != 4 or fields[0] == 'library':
        continue
    library, nid, stub, _ = fields
    name = by_stub.get(int(stub, 16) + base)
    if name and not name.startswith('Func') and (library, nid) not in known:
        added.append(f"{library}\t{nid}\t{name}")

with open(db_path, 'a', encoding='utf8') as db:
    for row in added:
        db.write(row + '\n')
print(f"added {len(added)} names from {log_path}")
