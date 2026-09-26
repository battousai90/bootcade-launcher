#!/usr/bin/env python3
"""Compare what Bootcade expects a DAT's archives to hold with a reference DAT.

The engine's side comes from the launcher itself:

    BOOTCADE_DAT_LAYOUT=<loaded DAT file>:<mode>:<out.txt> bootcade

where <mode> is split, non-merged, merged, or empty (the DAT as it is).
The reference is any Logiqx DAT whose sets already list, one by one, what each
archive holds (a DAT built for that mode by another ROM manager). Nothing here
depends on who published it.

    check-dat-layout.py out.txt reference.dat [--disks]

Compares archive names, then each archive's entries as sets of (path, crc) --
or (name, sha1) of its CHDs with --disks. Paths are compared with '/' whatever
the DAT spells. Exits 1 when anything differs.
"""
import sys
import xml.etree.ElementTree as ET


def engine(path, key):
    out, cur = {}, None
    for line in open(path, encoding="utf-8"):
        t = line.rstrip("\n").split("\t")
        if t[0] == "A":
            cur = out.setdefault(t[1], set())
        elif (t[0] == "E" and key == "roms") or (t[0] == "D" and key == "disks"):
            cur.add((t[1], t[2].lower()))
    return {k: v for k, v in out.items() if v}


def reference(path, key):
    out = {}
    tag, attr = ("rom", "crc") if key == "roms" else ("disk", "sha1")
    for _, el in ET.iterparse(path, events=("end",)):
        if el.tag not in ("machine", "game"):
            continue
        entries = {(e.get("name").replace("\\", "/"), (e.get(attr) or "").lower())
                   for e in el.findall(tag) if e.get(attr)}
        if entries:
            out[el.get("name")] = entries
        el.clear()
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2
    key = "disks" if "--disks" in sys.argv else "roms"
    ours, ref = engine(args[0], key), reference(args[1], key)
    only_ours, only_ref = sorted(set(ours) - set(ref)), sorted(set(ref) - set(ours))
    differ = sorted(k for k in set(ours) & set(ref) if ours[k] != ref[k])
    print(f"{key}: engine {len(ours)} archives, reference {len(ref)}; "
          f"only engine {len(only_ours)}, only reference {len(only_ref)}, content differs {len(differ)}")
    for k in only_ours[:10]:
        print("  only engine   ", k)
    for k in only_ref[:10]:
        print("  only reference", k)
    for k in differ[:10]:
        print("  differs       ", k, "engine-ref", sorted(ours[k] - ref[k])[:3], "ref-engine", sorted(ref[k] - ours[k])[:3])
    return 1 if (only_ours or only_ref or differ) else 0


if __name__ == "__main__":
    sys.exit(main())
