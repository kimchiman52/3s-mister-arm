#!/usr/bin/env python3
"""Generate src/netplay/plw_canon_fields.h — the canonical PLW hash field table.

The netplay desync checksum hashes PLW through an explicit member list rather
than raw struct bytes, because sizeof(PLW) is architecture-dependent (1092 on
armv7, 1304 on 64-bit: 49 pointer slots at 4 vs 8 bytes plus different padding).
Hashing an explicit list of the non-pointer members in declaration order gives a
byte image whose size and shape are identical on both architectures.

The list is derived from clang's own record layout for PLW, so it cannot drift
from the struct by hand-editing:

    tools/netplay/gen_plw_canon_fields.py --check     # verify header is current
    tools/netplay/gen_plw_canon_fields.py --write     # regenerate the header

Two targets are dumped (armv7 32-bit and the host 64-bit compiler) and the
resulting member-path lists are required to be identical; a difference would
mean a member exists on only one architecture, which the canonical image cannot
represent.

Aggregate members are descended into only when they are unions (whose branches
overlap) — every other nested struct is emitted whole, and the generated
header's own coverage assertions (see src/netplay/test_gs_coverage.c) prove the
result tiles PLW exactly.
"""

import argparse
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(REPO, "src", "netplay", "plw_canon_fields.h")

LINE_RE = re.compile(r"^\s*(\d+)\s*\|(\s+)(.*)$")
SIZE_RE = re.compile(r"^\s*\|\s*\[sizeof=(\d+), align=(\d+)\]")


def dump_layout(target):
    """Run clang -fdump-record-layouts for PLW against `target` (or host)."""
    src = "#include \"structs.h\"\nunsigned long plw_size = sizeof(PLW);\n"
    cmd = ["clang", "-fsyntax-only", "-Xclang", "-fdump-record-layouts",
           "-I" + os.path.join(REPO, "include"), "-I" + os.path.join(REPO, "src"),
           "-x", "c", "-"]
    if target:
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True,
                             text=True).stdout.strip()
        cmd = cmd[:1] + ["-target", target] + cmd[1:]
        if sdk:
            # Cross-target syntax-only parse still needs libc headers for the
            # typedefs in include/types.h; the host SDK's are pure typedefs.
            cmd += ["-isystem", os.path.join(sdk, "usr", "include")]
    out = subprocess.run(cmd, input=src, capture_output=True, text=True).stdout
    if "| PLW" not in out:
        sys.exit("clang produced no PLW record layout for target=%s" % target)
    return out


def parse_plw(dump):
    lines = dump.splitlines()
    start = max(i for i, ln in enumerate(lines) if re.match(r"^\s*0\s*\|\s*PLW$", ln))
    block, total = [], None
    for ln in lines[start + 1:]:
        m = SIZE_RE.match(ln)
        if m:
            total = int(m.group(1))
            break
        block.append(ln)
    nodes = []
    for ln in block:
        m = LINE_RE.match(ln)
        if not m:
            continue
        typ, name = m.group(3).rstrip().rsplit(" ", 1)
        nodes.append([len(m.group(2)), int(m.group(1)), typ, name, False])
    for i, n in enumerate(nodes):
        if i + 1 < len(nodes) and nodes[i + 1][0] > n[0]:
            n[4] = True

    members, stack, skip_indent = [], [], None
    for indent, off, typ, name, has_kids in nodes:
        if skip_indent is not None:
            if indent > skip_indent:
                continue
            skip_indent = None
        while stack and stack[-1][0] >= indent:
            stack.pop()
        path = ".".join([s[1] for s in stack] + [name])
        if has_kids and not typ.startswith("union "):
            stack.append((indent, name))
            continue
        members.append((path, off, typ))
        if has_kids:
            skip_indent = indent
    return members, total


def collect():
    arm, arm_total = parse_plw(dump_layout("armv7-unknown-linux-gnueabihf"))
    host, host_total = parse_plw(dump_layout(None))
    if [m[0] for m in arm] != [m[0] for m in host]:
        sys.exit("member paths differ between armv7 and host layouts")
    fields = [m for m in arm if "*" not in m[2]]
    pointers = [m for m in arm if "*" in m[2]]
    return fields, pointers, arm_total, host_total


def render(fields, pointers, arm_total, host_total):
    out = []
    out.append("/* GENERATED FILE — do not edit by hand.")
    out.append(" *")
    out.append(" * Regenerate with tools/netplay/gen_plw_canon_fields.py --write, which")
    out.append(" * derives this list from clang's record layout for PLW on both armv7 and")
    out.append(" * the host 64-bit target and requires the two to agree.")
    out.append(" *")
    out.append(" * PLW_CANON_FIELD_LIST is every non-pointer member of PLW in declaration")
    out.append(" * order; PLW_CANON_POINTER_LIST is every pointer member. Pointers are")
    out.append(" * excluded from the checksum because their VALUES are meaningless across")
    out.append(" * peers (ASLR) and their WIDTHS differ across architectures; they are")
    out.append(" * listed so the coverage proof can account for every byte of the struct.")
    out.append(" *")
    out.append(" * At generation time: sizeof(PLW) = %d (armv7), %d (64-bit);" % (arm_total, host_total))
    out.append(" * %d non-pointer members, %d pointer members." % (len(fields), len(pointers)))
    out.append(" */")
    out.append("")
    out.append("#ifndef PLW_CANON_FIELDS_H")
    out.append("#define PLW_CANON_FIELDS_H")
    out.append("")
    out.append("#define PLW_CANON_FIELD_LIST(X) \\")
    for i, (path, _off, _typ) in enumerate(fields):
        out.append("    X(%s)%s" % (path, " \\" if i + 1 < len(fields) else ""))
    out.append("")
    out.append("#define PLW_CANON_POINTER_LIST(X) \\")
    for i, (path, _off, _typ) in enumerate(pointers):
        out.append("    X(%s)%s" % (path, " \\" if i + 1 < len(pointers) else ""))
    out.append("")
    out.append("#endif // PLW_CANON_FIELDS_H")
    out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    fields, pointers, arm_total, host_total = collect()
    text = render(fields, pointers, arm_total, host_total)

    if args.write:
        with open(HEADER, "w") as f:
            f.write(text)
        print("wrote %s (%d fields, %d pointers)" % (HEADER, len(fields), len(pointers)))
        return 0

    current = open(HEADER).read() if os.path.exists(HEADER) else ""
    if current != text:
        print("STALE: %s does not match the struct layout; rerun with --write" % HEADER)
        return 1
    print("OK: %s matches PLW layout (%d fields, %d pointers, sizeof=%d/%d)"
          % (HEADER, len(fields), len(pointers), arm_total, host_total))
    return 0


sys.exit(main())
