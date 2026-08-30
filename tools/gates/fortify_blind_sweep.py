#!/usr/bin/env python3
"""Re-compile every translation unit with the host's libc fortification off.

WHY THIS EXISTS
===============

The consolidated merge at ed37cb42 passed every gate this project runs -- nine
netplay harnesses, the shipped-config host build, the frame-data suite at 94
GREEN -- and then failed to cross-compile on the first ARM build:

    src/netplay/netplay_nav.c:445:13: error: 'snprintf' will always be  doccite:quote
    truncated; specified size is 192, but format string expands to at least
    227 [-Werror,-Wformat-truncation]

(That is the compiler's own output against ed37cb42, quoted verbatim, so the
line number is the one ed37cb42 had. The fix that followed inserted ten lines
above the call, so the same statement is lower in the file today. The
`doccite:quote` marker tells tools/doc-citations/check_doc_citations.py that
this is a transcript of what a tool said, not a live claim about where the code
is now -- without it, a verbatim quote of a diagnostic becomes a citation that
rots the moment the file it names is edited.)

The obvious explanation -- "the ARM container runs clang-20 and the host runs
AppleClang, so the compilers simply disagree" -- is WRONG, and acting on it
would have produced a gate that does not work. Measured, on this machine:

    probe: a 192-byte buffer and a 227-byte format string, no #include,
           snprintf declared by hand with __attribute__((format(printf,3,4)))

    AppleClang 21.0.0 (clang-2100.1.1.101)  -Wall  -> WARNS
    Debian clang 20.1.8 (the ARM container)  -Wall  -> WARNS
    Homebrew clang 22.1.8                    -Wall  -> WARNS

All three compilers implement -Wformat-truncation and all three enable it
under -Wall. The compilers agree completely. What differs is the LIBC HEADER.

On Darwin, <stdio.h> pulls in <secure/_stdio.h>, which under the default
_FORTIFY_SOURCE macro-replaces the call:

    $ cc -E -                       # snprintf(b, 10, "x")
    __builtin___snprintf_chk (b, 10, 0, __builtin_object_size (b, ...), "x")

    (the macro is at, e.g., MacOSX.sdk/usr/include/secure/_stdio.h:74-77)

Clang's -Wformat-truncation check keys on the `snprintf` builtin. Once the
preprocessor has rewritten the call to `__builtin___snprintf_chk`, the check
never runs. glibc does not do this rewrite, so the same source warns on Linux
and does not warn on macOS -- with the SAME compiler.

Proof that the header, not the compiler, is the whole story:

    $ cc -Wall -fsyntax-only probe.c                       # silent
    $ cc -Wall -D_FORTIFY_SOURCE=0 -fsyntax-only probe.c   # WARNS

CONSEQUENCE FOR THE GATE
========================

Adding -Wformat-truncation to the host build would be a no-op: -Wall already
enables it, and the diagnostic is suppressed one layer below the flag. There
is no warning flag that fixes this. The only host-side lever is to compile the
tree once with fortification disabled -- which is what this script does.

That is a diagnostic sweep, not a build. It uses -fsyntax-only and produces no
object files, so nothing shipped is affected: _FORTIFY_SOURCE stays on for
every artifact that is actually linked and run. The sweep exists purely to let
the host SEE a class of defect its own headers hide.

Measured cost and signal on the tree at ed37cb42, shipped config, 662 TUs:

    fortification on  (baseline)        0 diagnostics
    fortification off                   1 diagnostic  -- netplay_nav.c:445,
                                        the exact -Wformat-truncation the ARM
                                        build failed on

One true positive, zero false positives, whole tree. See --control for the
non-vacuity check that keeps this from silently degrading into a no-op.

USAGE
=====

    tools/gates/host-diagnostic-parity.sh          # the normal entry point

    python3 tools/gates/fortify_blind_sweep.py --compile-db <path>
        [--baseline]   also run the sweep with fortification ON and report the
                       delta, rather than just the fortification-OFF findings
        [--control]    run the non-vacuity control and exit
        [--jobs N]

Exit codes: 0 clean, 1 diagnostics found, 2 harness error.
"""

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

# The lever. -U first because a compile command is free to define it already;
# redefining without undefining is itself a warning under -Werror.
FORTIFY_OFF = ["-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0"]

RE_DIAG = re.compile(r"^(.*?):(\d+):(\d+): (warning|error): (.*)$")

# A format string longer than the buffer, in the same shape as the defect this
# gate was built for. Used by --control.
CONTROL_SRC = """#include <stdio.h>
int fortify_blind_control(int a) {
    char buf[8];
    snprintf(buf, sizeof(buf),
             "this literal is comfortably longer than eight bytes (%d)", a);
    return (int)buf[0];
}
"""


def strip_output_args(argv):
    """Turn a compile command into a syntax-only command.

    Drops `-c` and `-o <path>` so nothing is written, and drops `-Werror` so
    the sweep can report every finding in one pass instead of stopping at the
    first. Severity is decided here, not by the compiler: any diagnostic at all
    fails this gate.
    """
    out = []
    skip = False
    for arg in argv:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
            continue
        if arg == "-c" or arg == "-Werror":
            continue
        out.append(arg)
    return out


def compile_one(entry, extra, cwd_default):
    argv = strip_output_args(shlex.split(entry["command"]))
    argv += ["-fsyntax-only", "-Wno-error"] + extra
    proc = subprocess.run(
        argv,
        cwd=entry.get("directory", cwd_default),
        capture_output=True,
        text=True,
    )
    return entry["file"], proc.stderr, proc.returncode


def diagnostics(stderr):
    """Extract just the diagnostic headline lines, dropping source echoes."""
    return [ln for ln in stderr.splitlines() if RE_DIAG.match(ln)]


def sweep(db, extra, jobs, cwd_default):
    found = []
    hard_errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [
            pool.submit(compile_one, e, extra, cwd_default) for e in db
        ]
        for fut in concurrent.futures.as_completed(futures):
            path, stderr, rc = fut.result()
            diags = diagnostics(stderr)
            found.extend(diags)
            # A nonzero exit with no parsed diagnostic means the sweep itself
            # is broken (bad flags, missing header, compiler not found). That
            # is a harness error, not a finding -- reporting it as "clean"
            # would be the exact silent-no-op failure this gate guards against.
            if rc != 0 and not any("error:" in d for d in diags):
                hard_errors.append("%s: exit %d\n%s" % (path, rc, stderr))
    return sorted(set(found)), hard_errors


def group_of(diag):
    m = re.search(r"\[-W([a-z0-9-]+)\]", diag)
    return "-W" + m.group(1) if m else "(ungrouped)"


def run_control(cc, jobs):
    """Prove the sweep can still see the defect class it was built for.

    A sweep that has quietly stopped compiling anything -- wrong flag, empty
    compile database, a compiler that no longer implements the check -- reports
    zero findings, which is indistinguishable from success. This compiles a
    deliberately-truncating snprintf and requires that fortification-off catches
    it and fortification-on does not. Both halves matter: the first says the
    check works, the second says the blindness being compensated for is real.
    """
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "control.c")
        with open(src, "w") as fh:
            fh.write(CONTROL_SRC)
        base = [cc, "-Wall", "-fsyntax-only", src]
        on = subprocess.run(base, capture_output=True, text=True)
        off = subprocess.run(base + FORTIFY_OFF, capture_output=True, text=True)

    on_hit = "format-truncation" in on.stderr
    off_hit = "format-truncation" in off.stderr

    print("non-vacuity control (%s):" % cc)
    print("  fortification ON  -> %s" % ("DIAGNOSED" if on_hit else "silent"))
    print("  fortification OFF -> %s" % ("DIAGNOSED" if off_hit else "silent"))

    if not off_hit:
        print("CONTROL FAILED: the sweep cannot see a deliberately-truncating "
              "snprintf even with fortification off. This gate would pass "
              "vacuously; fix it before trusting a green result.", file=sys.stderr)
        return 2
    if on_hit:
        print("CONTROL NOTE: this host is NOT blind to -Wformat-truncation "
              "with fortification on. The sweep is harmless but no longer "
              "load-bearing here; the host build already sees this class.")
    print("CONTROL OK")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--compile-db", required=False,
                    help="path to compile_commands.json")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--baseline", action="store_true",
                    help="also sweep with fortification ON and print the delta")
    ap.add_argument("--control", action="store_true",
                    help="run the non-vacuity control and exit")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = ap.parse_args(argv)

    if args.control:
        return run_control(args.cc, args.jobs)

    if not args.compile_db:
        print("--compile-db is required unless --control is given",
              file=sys.stderr)
        return 2
    try:
        with open(args.compile_db) as fh:
            db = json.load(fh)
    except OSError as exc:
        print("cannot read compile database: %s" % exc, file=sys.stderr)
        return 2

    if not db:
        print("compile database is empty -- refusing to pass vacuously",
              file=sys.stderr)
        return 2

    cwd_default = os.path.dirname(os.path.abspath(args.compile_db))

    off, off_errs = sweep(db, FORTIFY_OFF, args.jobs, cwd_default)
    if off_errs:
        print("HARNESS ERROR: %d translation unit(s) failed to compile for "
              "reasons that are not diagnostics:" % len(off_errs),
              file=sys.stderr)
        for e in off_errs[:5]:
            print(e, file=sys.stderr)
        return 2

    base = []
    if args.baseline:
        base, base_errs = sweep(db, [], args.jobs, cwd_default)
        if base_errs:
            print("HARNESS ERROR in baseline sweep: %d TU(s)" % len(base_errs),
                  file=sys.stderr)
            return 2
        hidden = [d for d in off if d not in set(base)]
        print("baseline (fortification ON):  %d diagnostic(s)" % len(base))
        print("sweep    (fortification OFF): %d diagnostic(s)" % len(off))
        print("HIDDEN BY THE HOST'S OWN HEADERS: %d" % len(hidden))
        for d in hidden:
            print("  " + d)
        by_group = {}
        for d in hidden:
            by_group.setdefault(group_of(d), 0)
            by_group[group_of(d)] += 1
        for g, n in sorted(by_group.items()):
            print("  group %s: %d" % (g, n))

    for d in off:
        print(d)

    print("FORTIFY-BLIND SWEEP: tus=%d diagnostics=%d %s"
          % (len(db), len(off),
             " ".join("%s=%d" % (g, sum(1 for d in off if group_of(d) == g))
                      for g in sorted({group_of(d) for d in off}))))
    return 1 if off else 0


if __name__ == "__main__":
    sys.exit(main())
