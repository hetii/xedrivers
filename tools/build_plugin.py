#!/usr/bin/env python3
"""Pack a plugin that imports from xam.xex as well as the kernel.

The kit's `plugin.py` emits one import library, the kernel's. `write.py` under it
is already general -- `build_xex` takes a module tuple and one record each -- so
this adds the second record rather than changing either: kit/ stays a copy of
xerunner's, and everything mechanical is reused from it.

Each import's name is looked up in the kernel's table first and then in xam's, so
a call added to the sources needs nothing here. A name in neither fails the build,
and a name in both is refused rather than guessed at.

The two identities the loader matches a module against were read out of this
console's own plugins (`xex list-imports` on ftpdll.xex and xbdm.xex, whose tables
carry them raw): the kernel's 0x45DC17E0 and xam's 0xFCA15C76, both with the same
version words the kit already uses.

    build_plugin.py SOURCES... --into DIR --ordinals KERNEL.txt --xam XAM.txt
                    [--base ADDR] [--name NAME] [-I DIR] [-D DEFINE]
"""

import argparse
import itertools
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "kit"))
import plugin as kit                                              # noqa: E402
import write                                                      # noqa: E402

KERNEL_ID = 0x45DC17E0
XAM_ID = 0xFCA15C76
VERSION = 0x20518700
LOWEST = 0x20074500


def build(sources, into, kernel_table, xam_table, base, name, includes, defines):
    missing = [one for one, here in kit.available().items() if not here]
    if missing:
        raise ValueError("%s not installed" % ", ".join(kit.TOOLS[one]
                                                        for one in missing))
    os.makedirs(into, exist_ok=True)
    flags = kit.FLAGS + ["-I" + one for one in includes] + ["-D" + one
                                                            for one in defines]
    objects = []
    for source in [*sources, os.path.join(os.path.dirname(HERE), "kit", "kcall.S")]:
        out = os.path.join(into, os.path.basename(source) + ".o")
        kit._run([kit.TOOLS["gcc"], *flags, "-c", source, "-o", out])
        objects.append(out)

    defined, undefined = set(), set()
    for one in objects:
        held, wanted = kit._symbols(kit.TOOLS["nm"], one)
        defined |= held
        undefined |= wanted
    wanted = sorted(undefined - defined)

    both = [one for one in wanted if one in kernel_table and one in xam_table]
    if both:
        raise ValueError("named by both tables, so which module is a guess: %s"
                         % ", ".join(both))
    unknown = [one for one in wanted if one not in kernel_table
               and one not in xam_table]
    if unknown:
        raise ValueError("no ordinal in either table for: %s" % ", ".join(unknown))

    # Kernel imports first, then xam's; the address table, the thunks and each
    # record's address list all follow this one order.
    imports = [(one, kernel_table[one], 0) for one in wanted if one in kernel_table]
    imports += [(one, xam_table[one], 1) for one in wanted if one in xam_table]

    pairs = [(one, ordinal) for one, ordinal, _ in imports]
    kit._run([kit.TOOLS["gcc"], *flags, "-c", kit._thunks(pairs, into),
              "-o", os.path.join(into, "thunks.o")])
    objects.append(os.path.join(into, "thunks.o"))

    elf = os.path.join(into, "plugin.elf")
    iat = b"".join(struct.pack(">I", ordinal) for _, ordinal, _ in imports)

    def linked(data):
        if os.path.exists(elf):
            os.remove(elf)
        kit._run([kit.TOOLS["ld"], "-T", kit._script(base, data, into), *objects,
                  "-o", elf])
        return (kit._section(kit.TOOLS["objcopy"], elf, ".text"),
                kit._section(kit.TOOLS["objcopy"], elf, ".data"))

    text, _ = linked(base + 0x1000 + 0x100000)
    iat_rva = 0x1000 + -(-len(text) // 0x1000) * 0x1000
    data_rva = iat_rva + -(-len(iat) // 0x1000) * 0x1000
    grown, data = linked(base + data_rva)
    if len(grown) != len(text):
        raise ValueError("the code changed size between the two link passes")
    held = kit._addresses(kit.TOOLS["nm"], elf)
    if "DllMain" not in held:
        raise ValueError("this plugin defines no DllMain")

    sections = [write.Section(".text", grown, 0x1000, 0x60000020),
                write.Section(".rdata", iat, iat_rva, 0x40000040)]
    if data:
        sections.append(write.Section(".data", data, data_rva, 0xC0000040))
    above = sorted(one for one in held.values() if one > held["DllMain"])
    entries = [(held[one] - base, 0x10) for one, _, _ in imports if one in held]
    entries.append((held["DllMain"] - base,
                    max(8, above[0] - held["DllMain"] if above else 8)))
    pdata = b"".join(kit._runtime(rva, length) for rva, length in sorted(entries))
    pdata_rva = max(one.rva + one.span for one in sections)
    sections.append(write.Section(".pdata", pdata, pdata_rva, 0x40000040))
    end = max(one.rva + one.span for one in sections)
    if end < write.SMALLEST:
        sections.append(write.Section(".pad", bytes(write.SMALLEST - end), end,
                                      0xC0000040))
    ordered = sorted(sections, key=lambda one: one.rva)
    for earlier, later in itertools.pairwise(ordered):
        if earlier.rva + earlier.span > later.rva:
            raise ValueError("%s and %s overlap" % (earlier.name, later.name))

    image = write.build_pe(sections, base, held["DllMain"] - base,
                           {3: (pdata_rva, len(pdata))})
    # One record a module, in the module tuple's order; each lists the slots and
    # thunks of its own imports.
    per = {0: [], 1: []}
    for index, (one, _, library) in enumerate(imports):
        per[library] += [base + iat_rva + index * 4, held.get(one, 0)]
    records = [write.library(0, per[0], KERNEL_ID, VERSION, LOWEST),
               write.library(1, per[1], XAM_ID, VERSION, LOWEST)]
    blob = write.build_xex(image, base, held["DllMain"], sections, records,
                           modules=("xboxkrnl.exe", "xam.xex"), name=name)
    out = os.path.join(into, "plugin.xex")
    with open(out, "wb") as handle:
        handle.write(blob)
    return {"out": out, "elf": elf, "imports": imports,
            "kernel": len(per[0]) // 2, "xam": len(per[1]) // 2,
            "image": len(image), "container": len(blob),
            "sections": [(one.name, one.rva, len(one.data)) for one in ordered]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sources", nargs="+")
    ap.add_argument("--into", required=True)
    ap.add_argument("--ordinals", required=True, help="the kernel's table")
    ap.add_argument("--xam", required=True, help="xam.xex's table")
    ap.add_argument("--base", type=lambda t: int(t, 0), default=kit.BASE)
    ap.add_argument("--name", default="plugin.dll")
    ap.add_argument("-I", dest="includes", action="append", default=[])
    ap.add_argument("-D", dest="defines", action="append", default=[])
    a = ap.parse_args()
    out = build(a.sources, a.into, kit.ordinals(a.ordinals), kit.ordinals(a.xam),
                a.base, a.name, a.includes, a.defines)
    print("out       %s" % out["out"])
    print("imports   %d from the kernel, %d from xam" % (out["kernel"], out["xam"]))
    print("image     %#x   container %#x" % (out["image"], out["container"]))
    print("sections  %s" % ", ".join("%s@%#x+%#x" % one for one in out["sections"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
