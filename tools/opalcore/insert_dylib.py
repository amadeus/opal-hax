#!/usr/bin/env python3
"""insert_dylib.py <macho> <dylib-install-path>
Adds an LC_LOAD_DYLIB command to the arm64 slice of a (fat or thin) Mach-O, using free space after the
existing load commands. Run on an UNSIGNED binary (codesign --remove-signature first); re-sign afterwards.
Idempotent: does nothing if the command is already present."""
import struct
import sys

LC_LOAD_DYLIB = 0xC
LC_SEGMENT_64 = 0x19
CPU_ARM64 = 0x0100000C


def patch_slice(buf, base, dylib):
    magic, cpu, _sub, _ftype, ncmds, sizeofcmds, _flags, _res = struct.unpack_from("<IiiIIIII", buf, base)
    assert magic == 0xFEEDFACF, "not a 64-bit Mach-O slice"
    off = base + 32
    min_sect = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, off)
        if cmd == LC_LOAD_DYLIB:
            name_off = struct.unpack_from("<I", buf, off + 8)[0]
            name = bytes(buf[off + name_off:off + size]).split(b"\0")[0].decode()
            if name == dylib:
                print("already present:", dylib)
                return False
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from("<I", buf, off + 64)[0]
            for s in range(nsects):
                soff = struct.unpack_from("<I", buf, off + 72 + s * 80 + 48)[0]
                if soff and (min_sect is None or soff < min_sect):
                    min_sect = soff
        off += size
    name = dylib.encode() + b"\0"
    cmdsize = (24 + len(name) + 7) & ~7
    end = base + 32 + sizeofcmds
    free = (base + min_sect) - end
    print(f"slice@{base:#x}: ncmds={ncmds} sizeofcmds={sizeofcmds} free={free} need={cmdsize}")
    assert free >= cmdsize, "not enough header padding"
    assert all(b == 0 for b in buf[end:end + cmdsize]), "padding not zero"
    cmd = struct.pack("<IIIIII", LC_LOAD_DYLIB, cmdsize, 24, 2, 0x10000, 0x10000) + name
    buf[end:end + cmdsize] = cmd.ljust(cmdsize, b"\0")
    struct.pack_into("<II", buf, base + 16, ncmds + 1, sizeofcmds + cmdsize)
    return True


def main():
    path, dylib = sys.argv[1], sys.argv[2]
    buf = bytearray(open(path, "rb").read())
    magic = struct.unpack_from(">I", buf, 0)[0]
    changed = False
    if magic == 0xCAFEBABE:
        n = struct.unpack_from(">I", buf, 4)[0]
        for i in range(n):
            cpu, _sub, offset, _size, _align = struct.unpack_from(">iiIII", buf, 8 + i * 20)
            if cpu == CPU_ARM64:
                changed |= patch_slice(buf, offset, dylib)
    else:
        changed = patch_slice(buf, 0, dylib)
    if changed:
        open(path, "wb").write(buf)
        print("patched", path)


if __name__ == "__main__":
    main()
