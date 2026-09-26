#!/usr/bin/env python3
"""Aggregate ESP-IDF linker map sections per object file."""
import re, sys, collections

path = sys.argv[1] if len(sys.argv) > 1 else "build/ctag-tbd.map"
secs = {}            # section name -> per-obj bytes
cur = None
order = []
pat = re.compile(r"^\s+(?:\*fill\*\s+)?0x[0-9a-f]{6,}\s+0x([0-9a-f]+)\s+(\S+\.a)\(([^)]*)\)\s*$")
secpat = re.compile(r"^(\.flash\.\S+|\.dram\.\S+|\.iram\.\S+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)")

with open(path, "r", errors="replace") as fh:
    for line in fh:
        m = secpat.match(line)
        if m:
            cur = m.group(1)
            secs.setdefault(cur, collections.Counter())
            continue
        if cur is None:
            continue
        if line.startswith("."):          # next top level section
            cur = None
            continue
        m = pat.match(line)
        if m:
            secs[cur][ (m.group(2), m.group(3)) ] += int(m.group(1), 16)

def report(sec, n=18):
    c = secs.get(sec)
    if not c:
        print(f"  (section {sec} not found)")
        return
    total = sum(c.values())
    print(f"\n{sec}: {total:,} bytes accounted")
    for (arch, obj), sz in c.most_common(n):
        print(f"  {sz:9,}  {arch.replace('lib','').replace('.a',''):24s} {obj}")

for s in (".flash.text", ".flash.rodata", ".flash.appdesc"):
    report(s)

# group flash text of the project's own archives by component
own = ("ctagSoundProcessor", "main", "drivers", "mutable")
agg = collections.Counter()
for s in (".flash.text", ".flash.rodata"):
    for (arch, obj), sz in secs.get(s, {}).items():
        import os
        name = os.path.basename(arch)[3:-2]        # libmain.a -> main
        if name in own and "resources.cc" not in obj:
            agg[(name, obj)] += sz
print("\nOwn code (.flash.text + .flash.rodata), top 30 objects:")
for (arch, obj), sz in agg.most_common(30):
    print(f"  {sz:9,}  {arch:22s} {obj}")

tot = collections.Counter()
for s in (".flash.text", ".flash.rodata"):
    for k, v in secs.get(s, {}).items():
        tot[k] += v
print("\nPer archive (.flash.text + .flash.rodata), top 15:")
import os
byarch = collections.Counter()
for (a, o), v in tot.items():
    byarch[os.path.basename(a)] += v
for arch, sz in byarch.most_common(15):
    print(f"  {sz:9,}  {arch}")
