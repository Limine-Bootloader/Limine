# Contributing to Limine

Thanks for wanting to contribute to the project. This document covers some requirements
and overall style and conventions that should be followed. Please read it carefully before
submitting contributions.

## Sign-off (DCO)

Every commit must be signed off by at least one (1) human contributor:

```
git commit -s
```

This adds a `Signed-off-by: Your Name <your@email>` trailer certifying that you wrote the
change (or otherwise **have reviewed** the commit and have the right to submit it) and agree
to contribute it under the project's license, per the Developer Certificate of Origin (the
[`DCO`](DCO) file in this repository - also at https://developercertificate.org).

## AI-assisted contributions

AI-assisted contributions are allowed, subject to the following:

- **A human must be in the loop at all times.** AI tools may assist, but a human contributor
  must drive the work, review and understand every generated change, and take full
  responsibility for it. Do not submit code you have not read and understood.
- **The assistance must be disclosed.** Any commit produced with material AI help *must*
  carry an `Assisted-by:` trailer naming the model, for example:

  ```
  Assisted-by: Claude:claude-opus-4-8
  ```

- The human contributor still signs off (see above). `Signed-off-by:` is the human's
  certification of, and responsibility for, the change. `Assisted-by:` only records which
  tool helped. It does not replace the sign-off or the human review.

Unreviewed, bulk, or fully-automated submissions are not accepted.

## C standard

For the bootloader proper, C99 with GNU extensions (AKA `gnu99`) and other common extensions
is used, where "common" means any extension that has been supported by both GCC and Clang
for a number of years (ideally 5 or more).

For build and host tools (i.e. C code under `tools/` and `host/`), strictly conforming C99
with no extensions must be used.

## Style

The project follows a relatively standard C coding style. It boils down to:

- No hard tabs. Spaces for indentation and alignment. 4-space per indentation level.
- Snake-case for most identifiers.
- Uppercase snake-case for macros.
- Same line curly braces for blocks, including for functions (i.e. `if (...) {`,
  `void func(void) {`, ...).
- Same line closing block brace and else (i.e. `} else {`).
- Comments are sparse: explain a non-obvious *why*, never restate the *what*. Don't narrate;
  avoid useless comments.
- Stick to ASCII: avoid em-dashes and other non-ASCII characters in code, comments, commit
  messages, and documentation, unless the non-ASCII character is essential to the work.
- Do not add per-file license headers.
- Do not edit vendored/fetched/generated files (i.e. anything in `3RDPARTY.md`, or not in
  `git ls-files`).
- As a catch-all, match the surrounding code: indentation, braces, naming, idiom. Mirror the
  conventions used by the file you edit.

## Commit conventions

- One logical change per commit. No "and" commits - split unrelated changes apart.
- Commit message: a `<area>: <imperative summary>` subject. No body unless expanding on the
  commit subject is considered important enough to improve clarity (or the "why" a commit
  was made); in any case the body should not be overly verbose. Try your best to keep both
  the subject and any body lines within 80-column terminals as `git log` shows them (it
  indents the message by 4 spaces, so aim for roughly 72 columns and wrap the body
  accordingly). `<area>` is often the path to the `{.c,.h}` pair inside `common/` (e.g.
  `lib/acpi:` or `drivers/gop:`), or sometimes more generic concepts such as `build:` or
  `docs:`. Host tools use `host/<name>:`, build-time tools use `tools/<name>:`, BIOS stage 1
  uses `stage1/{cd,hdd,pxe,decompressor,gdt}:`, and for everything else just follow whatever
  established convention.
