#!/usr/bin/env python3
"""Check that the authenticated-punch token compare is still constant-time.

WHY THIS IS A SOURCE CHECK AND NOT A UNIT TEST. Stun_IsPunchPayload
(src/netplay/stun.c) compares the 8-byte room-code-derived punch token with
an accumulator rather than a memcmp, on purpose:

    uint8_t diff = 0;
    for (int i = 0; i < STUN_PUNCH_TOKEN_LEN; i++) {
        diff |= (uint8_t)(buf[STUN_PUNCH_PREFIX_LEN + i] ^ token[i]);
    }
    return diff == 0;

The comment above it states the reason: "a UDP responder that early-exits on
the first mismatching byte is a (weak, but free-to-close) timing oracle for
guessing the token byte-by-byte."

An early-exiting rewrite is BEHAVIOURALLY IDENTICAL. It returns false for
every wrong token and true for the right one, so
src/netplay/test_punch_predicates.c -- which flips all 64 token bits and all
72 prefix bits -- passes against it unchanged, as does every integration
harness and the netns rig. There is no deterministic in-process assertion
that separates the two: a timing measurement over a 17-byte compare is noise
on any machine this project builds on, and a flaky gate is worse than none.

So the property is pinned where it is actually expressed: in the shape of
the loop. This checks that

  1. the loop still runs over the full token length (a `- 1`, a literal 7, or
     a sizeof on a pointer would silently stop comparing the tail);
  2. the loop body ACCUMULATES with |= rather than assigning, so no iteration
     can discard an earlier difference; and
  3. the loop body contains no early exit -- no return, break, goto, or
     conditional -- so every call does the same work regardless of where the
     first mismatching byte is.

Exit codes: 0 = the shape holds, 1 = it is violated, 2 = the function or its
loop could not be located (which is itself a failure -- a check that silently
finds nothing is worse than no check).
"""

import argparse
import pathlib
import re
import sys

TAG = "[constant-time-compare]"


def extract_function(text, signature_re):
    """Return (body, first_line_number) for the function whose definition line
    matches signature_re, by brace matching from its opening brace."""
    m = re.search(signature_re, text)
    if m is None:
        return None, None
    start = text.index("{", m.start())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i], text[:m.start()].count("\n") + 1
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stun", default="src/netplay/stun.c")
    args = ap.parse_args()

    path = pathlib.Path(args.stun)
    if not path.is_file():
        print(f"{TAG} ERROR: {path} does not exist", file=sys.stderr)
        return 2
    text = path.read_text()

    body, lineno = extract_function(
        text, r"^bool\s+Stun_IsPunchPayload\s*\(", )
    if body is None:
        # Signature may wrap; fall back to a looser anchor before giving up.
        body, lineno = extract_function(text, r"bool\s+Stun_IsPunchPayload\s*\(")
    if body is None:
        print(f"{TAG} ERROR: could not locate Stun_IsPunchPayload in {path} — "
              f"the function was renamed or moved, and this check now guards "
              f"nothing", file=sys.stderr)
        return 2

    loop = re.search(
        r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*([^;]+?)\s*;\s*i\s*\+\+\s*\)\s*\{"
        r"(.*?)\n\s*\}",
        body, re.S)
    if loop is None:
        print(f"{TAG} ERROR: Stun_IsPunchPayload (near {path}:{lineno}) no "
              f"longer contains the token-compare loop this check guards. If "
              f"the compare moved, point this check at its new home; do not "
              f"delete it.", file=sys.stderr)
        return 2

    bound, inner = loop.group(1).strip(), loop.group(2)
    failures = []

    # 1. the full token length, symbolically.
    if bound != "STUN_PUNCH_TOKEN_LEN":
        failures.append(
            f"the loop bound is `{bound}`, not STUN_PUNCH_TOKEN_LEN — the "
            f"compare no longer provably covers the whole token")

    # 2. accumulate, never assign.
    if "|=" not in inner:
        failures.append(
            "the loop body does not accumulate with `|=` — an assignment (or "
            "an xor fold) lets a later iteration erase an earlier difference")

    # 3. no early exit of any kind.
    for kw, why in (
        (r"\breturn\b", "returns from inside the loop"),
        (r"\bbreak\b", "breaks out of the loop"),
        (r"\bgoto\b", "jumps out of the loop"),
        (r"\bif\b", "branches inside the loop"),
        (r"\bcontinue\b", "skips iterations"),
        (r"\?", "uses a conditional operator inside the loop"),
        (r"&&|\|\|", "uses a short-circuiting operator inside the loop"),
    ):
        if re.search(kw, inner):
            failures.append(
                f"the loop body {why} — that is a data-dependent exit, i.e. "
                f"exactly the byte-by-byte timing oracle the compare exists "
                f"to close")

    if failures:
        print(f"{TAG} FAIL: the punch-token compare in {path} is no longer "
              f"constant-time.", file=sys.stderr)
        for f in failures:
            print(f"{TAG}   - {f}", file=sys.stderr)
        print(f"{TAG} No unit test can catch this: an early-exiting compare "
              f"returns exactly the same verdict for every input, so "
              f"src/netplay/test_punch_predicates.c stays green while a "
              f"remote attacker recovers the room token one byte at a time.",
              file=sys.stderr)
        return 1

    print(f"{TAG} PASS: Stun_IsPunchPayload still accumulates over all "
          f"{bound} bytes with no data-dependent exit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
