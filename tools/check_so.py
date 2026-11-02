#!/usr/bin/env python3
"""Check a built MPC plugin .so without any ARM toolchain.

    python3 tools/check_so.py mpc/obj/sdr_radio.so

Reads the ELF directly and checks what MPC OS needs: 32-bit ARM hard-float,
no glibc symbol newer than 2.31 (MPC OS 2.x), and nothing exported but
VSTPluginMain (so libraries linked in can't clash with MPC's own). Also lists
the shared libraries it needs. Exit status 1 on a failure.
"""
import struct
import sys

MAX_GLIBC = (2, 31)
EXPORTS = {"VSTPluginMain"}


def main(path):
    data = open(path, "rb").read()
    fail = []
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        print("error: not a 32-bit little-endian ELF")
        return 1
    e_machine, = struct.unpack_from("<H", data, 18)
    e_flags, = struct.unpack_from("<I", data, 36)
    if e_machine != 40:
        fail.append("not ARM (e_machine %d)" % e_machine)
    if not e_flags & 0x400:
        fail.append("not hard-float (EF_ARM_ABI_FLOAT_HARD missing)")

    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 46)
    secs = []
    for i in range(shnum):
        name, typ, _, _, off, size, link, info, _, entsize = struct.unpack_from("<10I", data, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, off=off, size=size, link=link, info=info, entsize=entsize))

    def cstr(sec, off):
        start = secs[sec]["off"] + off
        return data[start:data.index(b"\0", start)].decode()

    by_type = {}
    for i, s in enumerate(secs):
        by_type.setdefault(s["type"], i)
    dynsym = by_type.get(11)          # SHT_DYNSYM
    verneed = by_type.get(0x6ffffffe)  # SHT_GNU_verneed
    dynamic = by_type.get(6)          # SHT_DYNAMIC

    # Versions the .so needs, e.g. GLIBC_2.28.
    newest = (0, 0)
    if verneed is not None:
        s = secs[verneed]
        off = s["off"]
        for _ in range(s["info"]):
            _, cnt, _, aux, nxt = struct.unpack_from("<HHIII", data, off)
            a = off + aux
            for _ in range(cnt):
                _, _, _, name, anext = struct.unpack_from("<IHHII", data, a)
                v = cstr(s["link"], name)
                if v.startswith("GLIBC_"):
                    t = tuple(int(x) for x in v[6:].split(".")[:2])
                    newest = max(newest, t)
                a += anext
            if not nxt:
                break
            off += nxt
    print("newest glibc: %d.%d" % newest)
    if newest > MAX_GLIBC:
        fail.append("needs glibc %d.%d, MPC OS 2.x has %d.%d" % (newest + MAX_GLIBC))

    # Exported (defined, global/weak, default-visible) dynamic symbols.
    exported = []
    if dynsym is not None:
        s = secs[dynsym]
        for i in range(1, s["size"] // s["entsize"]):
            name, _, _, info, other, shndx = struct.unpack_from("<IIIBBH", data, s["off"] + i * s["entsize"])
            if shndx != 0 and info >> 4 in (1, 2) and other & 3 == 0:
                exported.append(cstr(s["link"], name))
    print("exports: " + ", ".join(sorted(exported)))
    extra = set(exported) - EXPORTS
    if extra:
        fail.append("exports more than %s: %s" % (sorted(EXPORTS), ", ".join(sorted(extra)[:10])))
    if not EXPORTS <= set(exported):
        fail.append("doesn't export %s" % sorted(EXPORTS - set(exported)))

    if dynamic is not None:
        s = secs[dynamic]
        needed = []
        for i in range(s["size"] // 8):
            tag, val = struct.unpack_from("<iI", data, s["off"] + i * 8)
            if tag == 1:
                needed.append(cstr(s["link"], val))
        print("needs: " + ", ".join(needed))

    for f in fail:
        print("error: " + f)
    if not fail:
        print("so check: OK")
    return 1 if fail else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
