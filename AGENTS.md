# AGENTS.md

Instructions for agents working in this repo. Keep changes minimal and in the style of the
surrounding code.

## Style

- Match the surrounding code: indentation, braces, naming, idiom. Mirror the file you edit.
- Comments are sparse: explain a non-obvious *why*, never restate the *what*. Don't narrate;
  drop useless comments.
- Stick to ASCII: avoid em-dashes and other non-ASCII characters in code, comments, and
  commit messages.
- Don't add per-file license headers (most `common/` files have none).
- Don't out-strict firmware: length/bounds-check to avoid OOB, but warn-and-continue (or skip
  the optimization) on non-conformant-but-harmless input rather than rejecting or panicking.
  Match what the OSes Limine boots do (e.g. Linux/ACPICA accept bad ACPI checksums).
- Don't edit vendored/fetched/generated files (anything in `3RDPARTY.md`, or not in
  `git ls-files`).

## Commits

- One logical change per commit; no "and" commits; split unrelated changes apart.
- Single-line message: a `<area>: <imperative summary>` subject, no body. `<area>` is the
  code's path, e.g. `lib/acpi:`, `drivers/gop:`, `mm/mtrr:`, `sys/cpu_riscv:`, `protos/linux:`,
  `build:`, `test:`. The host tool is `host/limine:`; build-time tools are `tools/<name>:`.
- For agent-made changes, add an `Assisted-by:` trailer (after a blank line) naming the model,
  e.g. `Assisted-by: Claude:claude-opus-4-8`.
- Don't add `Co-authored-by:` (or other authorship) lines for agent work; the `Assisted-by:`
  trailer already covers it.
