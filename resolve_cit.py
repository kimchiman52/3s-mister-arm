#!/usr/bin/env python3
"""Task #103 lane-private. Resolve conflicts that differ ONLY in citation line
numbers. Proves prose-identity (text with all digit-runs masked) before touching
anything; refuses any conflict where the prose actually differs."""
import re, sys

CONF = re.compile(
    r"^<<<<<<< [^\n]*\n(.*?)^=======\n(.*?)^>>>>>>> [^\n]*\n", re.M | re.S)


def mask(s):
    return re.sub(r"\d+", "#", s)


def main(paths, take):
    ok = True
    for p in paths:
        src = open(p, encoding="utf-8").read()
        out, pos, n, refused = [], 0, 0, 0
        for m in CONF.finditer(src):
            ours, theirs = m.group(1), m.group(2)
            n += 1
            out.append(src[pos:m.start()])
            if mask(ours) != mask(theirs):
                refused += 1
                ok = False
                print(f"REFUSED {p} conflict#{n}: prose differs, not just numbers")
                print("  OURS  :", ours.strip()[:160])
                print("  THEIRS:", theirs.strip()[:160])
                out.append(m.group(0))          # leave markers in place
            else:
                out.append(theirs if take == "theirs" else ours)
            pos = m.end()
        out.append(src[pos:])
        open(p, "w", encoding="utf-8").write("".join(out))
        print(f"{p}: {n} conflicts, {n-refused} resolved (took {take}), {refused} refused")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[2:], sys.argv[1]))
