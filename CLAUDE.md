Read [AGENTS.md](AGENTS.md) for project conventions, build commands, safety rules, and the memory index.

## Documentation rules (full text in AGENTS.md)

- **Read `docs/archive/` for *why*** — the decision taken, the alternative
  rejected, and above all the negative results: what was tried, measured, and
  did not work. That knowledge exists nowhere else; code cannot record the
  absence of a thing. **Never read it for current facts** — each file is
  stamped with the commit it was true at and is not maintained. Read the code.
- **Do not repoint citations in unenforced files.** The enforced set is
  `tools/doc-citations/baselines.txt` and nothing else. If a file is not in it,
  its line numbers are not your problem. `docs/archive/` is not scanned at all.
- **Don't write prose restating what code does — write the assertion instead.**
  A verifiable fact belongs in a test. Prose is for what a test cannot hold:
  negative results, operator procedure, external facts, and decisions.
