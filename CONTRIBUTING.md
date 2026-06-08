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
  carry an `Assisted-by:` trailer using a format similar to the Linux kernel's AI coding
  assistant format:

  ```
  Assisted-by: AGENT_NAME:MODEL_VERSION
  ```

  `AGENT_NAME` is the AI tool or framework (e.g. `Codex`, `Claude`, ...). `MODEL_VERSION`
  is the full model identifier, all lowercase, including any numeric, snapshot, or version
  suffix. Do not shorten it to the model family (e.g. use `gpt-5-5`, not `gpt-5`;
  use `claude-opus-4-8`, not `claude-opus`).
- Do not add `Co-authored-by:` trailers for the AI assistant. The `Assisted-by:` trailer
  already serves that purpose.
- The human contributor still signs off (see above). `Signed-off-by:` is the human's
  certification of, and responsibility for, the change. `Assisted-by:` only records which
  tool helped. It does not replace the sign-off or the human review.

Unreviewed, bulk, or fully-automated submissions are not accepted.

## C standard

For the bootloader proper, C11 with GNU extensions (AKA `gnu11`) and other common extensions
is used, where "common" means any extension that has been supported by both GCC and Clang
for a number of years (ideally 5 or more).

For build and host tools (i.e. C code under `tools/` and `host/`), strictly conforming C99
with no extensions must be used.

## Style

### Generic style

- Use British spelling.
- No hard tabs. Spaces for indentation and alignment. 4-space per indentation level.
- Always avoid vertical alignment to minimise vertical blast radius on changes.
- Comments are sparse: explain a non-obvious *why*, never restate the *what*. Don't narrate;
  avoid useless comments.
- Stick to ASCII: avoid em-dashes and other non-ASCII characters in code, comments, commit
  messages, and documentation, unless the non-ASCII character is essential to the work.
- Do not add per-file license headers.
- Do not edit vendored/fetched/generated files (i.e. anything in `3RDPARTY.md`, or not in
  `git ls-files`).
- As a catch-all, match the surrounding code: indentation, braces, naming, idiom. Mirror the
  conventions used by the file you edit.

### C language specific style

The project follows a relatively standard C coding style. It boils down to:

- Snake-case for most identifiers.
- Uppercase snake-case for macros.
- No pointless `typedef`s, especially for `struct`s. Always use the full `struct name var;`
  declaration style for those.
- Right aligned pointers (i.e. `struct something *ptr`).
- Same line curly braces for blocks, including for functions (i.e. `if (...) {`,
  `void func(void) {`, ...).
- Same line closing block brace and else (i.e. `} else {`).
- Blocks should always be used for `if`, `for`, `while`, `do`, and similar. This means no
  blockless constructs such as:

  ```c
  if (condition)
      func();
  ```

  Instead, this should always be:

  ```c
  if (condition) {
      func();
  }
  ```

- For `switch`/`case` constructs, the indentation should look like this:

  ```c
  switch (variable) {
      case 'A': {
          func();
          break;
      }
      case 'B': {
          other_func();
          break;
      }
      default: {
          something_else();
          break;
      }
  }
  ```

  And, as shown, all cases should be curly braced blocks.
- **Never** do things such as `if()`, `while()`, ... - **Always** put a space between
  keywords (i.e. not functions/function pointers) and parentheses.

### Final style remarks

As with anything, there are always exceptions to these style rules, under given contexts.

## Commit conventions

- One logical change per commit. No "and" commits - split unrelated changes apart.
- Commit message: a `<area>: <imperative summary>` subject. Most commits should be
  subject-only; add a body only when the change has a non-obvious rationale or detail that
  genuinely does not fit in the subject, and keep it brief. Try your best to keep both the
  subject and any body lines within 80-column terminals as `git log` shows them (it indents
  the message by 4 spaces, so aim for roughly 72 columns and wrap the body accordingly).
  The `<imperative summary>` should begin with a uppercase letter, while `<area>` should be
  all lowercase.
  `<area>` is often the path to the `{.c,.h}` pair inside `common/` (e.g.
  `lib/acpi:` or `drivers/gop:`), or sometimes more generic concepts such as `build:` or
  `docs:`. Host tools use `host/<name>:`, build-time tools use `tools/<name>:`, BIOS stage 1
  uses `stage1/{cd,hdd,pxe,decompressor,gdt}:`, and for everything else just follow whatever
  established convention.
