"""Prints a Windows DLL's processor, its imports and its delay-loaded imports (CI).

    python -I pe-imports.py <file.dll> [--no-delay]

--no-delay: exit 1 when the file has delay-loaded imports. The service removes
the folder it adds to the DLL search right after loading ONNX Runtime
(service/src/common/platform.cpp), which is only safe for a library that loads
nothing later.
"""
import struct
import sys

MACHINES = {0x8664: "x64", 0xAA64: "arm64", 0x014C: "x86"}


def main():
    path = sys.argv[1]
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    machine = struct.unpack_from("<H", d, pe + 4)[0]
    sections = struct.unpack_from("<H", d, pe + 6)[0]
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    dirs = opt + (112 if struct.unpack_from("<H", d, opt)[0] == 0x20B else 96)
    table = []
    s = opt + opt_size
    for _ in range(sections):
        size, va, raw_size, raw = struct.unpack_from("<IIII", d, s + 8)
        table.append((va, max(size, raw_size), raw))
        s += 40

    def offset(rva):
        for va, size, raw in table:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"rva {rva:#x} outside the sections")

    def name(rva):
        o = offset(rva)
        return d[o:d.index(b"\0", o)].decode()

    def names(rva, stride, field):
        out = []
        if rva:
            o = offset(rva)
            while struct.unpack_from("<I", d, o + field)[0]:
                out.append(name(struct.unpack_from("<I", d, o + field)[0]))
                o += stride
        return out

    imports = names(struct.unpack_from("<I", d, dirs + 8)[0], 20, 12)
    delayed = names(struct.unpack_from("<I", d, dirs + 13 * 8)[0], 32, 4)
    print(f"{path}: {MACHINES.get(machine, hex(machine))}")
    print("imports: " + ", ".join(imports))
    print("delay-loaded: " + (", ".join(delayed) or "none"))
    if "--no-delay" in sys.argv[2:] and delayed:
        sys.exit(1)


main()
