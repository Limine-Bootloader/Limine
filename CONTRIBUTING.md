# Contributing to Limine

Thanks for wanting to contribute to the project. This document covers some
requirements and overall style and conventions that should be followed. Please
read it carefully before submitting contributions.

## AI-assisted contributions

AI-assisted contributions are allowed, subject to the following:

- **A human must be in the loop at all times.** AI tools may assist, but a
  human contributor must drive the work, review and understand every generated
  change, and take full responsibility for it. Do not submit code you have not
  read and understood.
- **The assistance must be disclosed, in exactly this form.** Every commit
  produced with material AI help *must* carry this trailer, one for each model
  used, among the trailers that end its message:

  ```
  Assisted-by: AGENT_NAME:MODEL_VERSION
  ```

  `AGENT_NAME` is the AI tool or harness, as it names itself (e.g. `Claude`,
  `Codex`, `Antigravity`). `MODEL_VERSION` is the model's exact identifier, as
  its provider names it, all lowercase, including any numeric, snapshot, or
  version suffix (e.g. `claude-opus-5-5`, `gpt-6-astra`, `gemini-4-argon`).
  Do not shorten it to the model family (e.g. `claude-opus`), and do not add
  anything that is not part of it, such as a reasoning effort (e.g. `-high`)
  or a context size (e.g. `[1m]`). A commit missing the trailer, or carrying
  it in any other form, is not merged.
- Do not add `Co-authored-by:` trailers, "Generated with" lines, or any other
  credit for the AI assistant. The `Assisted-by:` trailer already serves that
  purpose.

Unreviewed, bulk, or fully-automated submissions are not accepted.

## C standard

For the bootloader proper, C11 with GNU extensions (AKA `gnu11`) and other
common extensions is used, where "common" means any extension that has been
supported by both GCC and Clang for a number of years (ideally 5 or more).

For build and host tools (i.e. C code under `tools/` and `host/`), strictly
conforming C99 with no extensions must be used.

## Style

### Generic style

- Use British spelling, in prose, comments, and identifiers alike, C's own
  terms included (e.g. "initialiser", "behaviour"), except for "license",
  which is conventionally spelled that way even as a noun, and "center". What
  a specification or an interface names keeps its spelling (e.g. `COLOR_SELECT`
  for a register a datasheet calls Color Select).
- Use the Oxford comma whenever possible: before the conjunction of a list's
  last item.
- No hard tabs. Spaces for indentation and alignment, 4 per indentation level.
- Never align separate lines into columns, whether statements, declarations,
  macro values, trailing comments, or line-continuation backslashes, so that a
  change never forces its neighbours to change too. Lining up a statement's own
  continuation lines, as wrapping it requires, is the one exception.
- Comments are sparse: explain a non-obvious *why*, never restate the *what*.
  Don't narrate; avoid useless comments. Be as terse as possible, in full
  sentences, with a capital letter and a full stop. A comment that labels a
  table entry, a field, or a group of definitions need not be a sentence.
- Comments describe the code as it is, never its history: they *must never*
  reference code that no longer exists in the codebase (i.e. a bug that was
  just fixed by removing/changing some code). Saying why an obvious
  alternative is not used is fine.
- Stick to ASCII: avoid em-dashes and other non-ASCII characters in code,
  comments, commit messages, and documentation, unless the non-ASCII character
  is essential to the work.
- Whenever a specification is referenced, in code, comments, commit messages,
  or documentation, name its version, revision, edition, or other identifier
  along with it, as in `UEFI 2.11 section 8.2.3`, `PXE 2.1, 4.4.5`, or
  `Intel VT-d 5.20 section 6.5.2`. Sections move between revisions, so a bare
  name does not say what is meant.
- Documentation keeps its lines to 79 columns, wherever it can.
- Do not add per-file license headers.
- Do not edit fetched, generated, or vendored files: anything not in
  `git ls-files`, which includes everything `./bootstrap` pulls in, and files
  checked in unchanged from another project, such as the autoconf-archive
  macros in `m4/`. Code adapted from other projects within the tree may be
  edited like any other, but keep its license notices intact.
- As a catch-all, match the surrounding code: indentation, braces, naming,
  idiom. Mirror the conventions used by the file you edit.

### C language specific style

The project follows a relatively standard C coding style. It boils down to
the following.

#### Layout

- Same line curly braces for blocks, including for functions, `struct`s, and
  `union`s (i.e. `if (...) {`, `void func(void) {`, `struct name {`).
- Same line closing brace and whatever continues the statement (i.e.
  `} else {`, `} while (...);`).
- Blocks should always be used for `if`, `else`, `for`, `while`, `do`, and
  similar, `else if` being the one chained form. This means no blockless
  constructs such as:

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

- A block's body goes on lines of its own. An empty block is `{}`, but one
  holding only a comment is not empty.
- No trailing whitespace, a newline at the end of every file, no runs of blank
  lines, and one blank line between functions.

#### Line length and wrapping

- Lines of code are at most 99 columns, and comments end by column 79, their
  markers and indentation included. A trailing comment that would run past it
  goes on its own line above the code.
- A function signature or call wraps only when it does not fit, breaking after
  its `(`, with as many arguments as fit on each line, one level in, and its
  `)` on a line of its own, followed by ` {` in a definition.
- An expression breaks before an operator, first at the one that binds
  loosest. The operator starts the continuation line, hanging so that what
  follows it lines up with the start of the expression, which in the condition
  of an `if` or `while` is just inside the `(`:

  ```c
  if (whatever && whatever_else
   && whatever_else_2) {

  bool changed = old != new_value || flags_changed
              || size_changed;
  ```

- A call never wraps inside the header of an `if`, `while`, or `for`, or
  inside a larger expression: its result goes into a local first.
- A `?:` too long for its line breaks before both its `?` and its `:`.
- A string literal too long for its line is split at word boundaries into
  adjacent literals, each lined up under the first.

#### Spacing

- **Never** do things such as `if()`, `while()`, ... - **Always** put a space
  between keywords (i.e. not functions/function pointers) and parentheses. The
  exceptions are `sizeof`, `alignof`, `alignas`, `typeof`, `static_assert`,
  `_Generic`, `__attribute__`, and the preprocessor's `defined`, in all their
  spellings, which are written without the space (i.e. `sizeof(x)`,
  `_Static_assert(...)`, `defined(X)`). `asm` is not among them
  (i.e. `asm volatile ("nop");`).
- Binary and ternary operators have a space on each side, and unary ones
  none. A comma takes a space after it and none before, and nothing goes
  inside parentheses or brackets, or around `.` and `->`.
- A cast has no space before what it casts (i.e. `(uint32_t)value`).
- Right aligned pointers (i.e. `struct something *ptr`).
- Brace-enclosed lists have spaces inside their braces (i.e. `{ a, b }`,
  `{ 0 }`), and a multi-line one has no trailing comma. A compound literal's
  brace follows its type with no space (i.e. `(struct point){ .x = 1 }`).

#### Naming

- Snake-case for identifiers and file names, and uppercase snake-case for
  macros.
- What a file shares with other files starts with the file's name (e.g.
  `lapic_read()` in `sys/lapic.c`), but for names a specification gives (e.g.
  `MSR_IA32_PAT`), standard and idiomatic ones (e.g. `memcpy()`, `rdmsr()`),
  and those another program's interface gives (e.g. `efi_main()`). A file's
  own statics and macros need no prefix.
- No variable shadowing, globals included.

#### Declarations

- No pointless `typedef`s, especially for `struct`s and `union`s. Always use
  the full `struct name var;` declaration style for those. Accepted exceptions
  are typedefs mirroring the names of an external specification, such as the
  PE/COFF structures in `common/lib/pe.c`, and the `pagemap_t` struct that
  `common/mm/vmm.h` defines for each architecture.
- Local variables are declared at their first use or at the top of their
  block.
- An empty parameter list is `(void)`. An unused parameter is cast to `void`,
  and an unused local variable goes.
- A function defined in a header is `static inline`.
- A type attribute follows the closing brace (i.e.
  `} __attribute__((packed));`). A function attribute leads its declaration,
  in which the storage class comes first, then `inline` and `noreturn`, then
  the type.

#### Types and literals

- Fixed-width types are for values with a fixed size. Counts, sizes in bytes,
  and indices are `size_t`, loop iterators included (a loop counting down is
  `for (size_t i = n; i-- > 0; )`), and addresses kept in integers are
  `uintptr_t`, but a hardware register's value, or a number that hardware
  defines, keeps the width the hardware gives it. Never bare `unsigned`.
- Truth values are `bool`, and a function that only succeeds or fails returns
  `true` for success.
- Only truth values are used as truth values. Anything else is compared
  (i.e. `p != NULL`, `(flags & FLAG) != 0`, `count == 0`, never `if (p)` or
  `if (!count)`), though `!!x` is a truth value.
- No `enum`s: use macros and integers instead.
- Hex literals are lowercase (i.e. `0xff`).
- No integer literal suffixes (`u`, `ull`, ...): cast to the fixed-width type
  instead (i.e. `(uint64_t)1 << 32`), and parenthesise a macro whose whole
  body is such a cast (i.e. `#define PAGE_SIZE ((size_t)0x1000)`). Constants
  `#if` or assembly also use are plain literals.
- `sizeof` takes the type, not the variable (i.e. `sizeof(struct something)`),
  but for arrays, anonymous types, and generic macros.
- No bit-fields, and no variable-length arrays.

#### Control flow

- For `switch`/`case` constructs, the indentation should look like this:

  ```c
  switch (variable) {
      case 'A': {
          func();
          break;
      }
      case 'B':
      case 'C': {
          other_func();
          break;
      }
      default: {
          something_else();
          break;
      }
  }
  ```

  And, as shown, all cases should be curly braced blocks, labels sharing a
  body stacked above one. A case falling through into the next ends its
  block with `__attribute__((fallthrough));`.
- `goto` is encouraged wherever it makes sense, especially in cleanup code.
  Its labels are never indented, and one needing an empty statement after it
  is `label:;`.
- A loop that runs forever is `for (;;)`. An empty part of a `for` leaves a
  space where it would be, but for the first (i.e. `for (; i < n; i++)`).

#### Preprocessor and headers

- Includes come in one block of groups, each sorted by path: the standard
  headers, then those of the libraries used, then the `.c` file's own header,
  then the rest of Limine's. All are included with `<>`.
- Every file includes each header that declares or defines something it uses,
  and no other.
- Include guards are `#ifndef` macros shaped after the header's path, its
  directories and file name separated with `__` (i.e. `SYS__CPU_H` for
  `<sys/cpu.h>`), never `#pragma once`.
- Directives nested in a conditional block holding nothing but directives are
  indented after the `#`, 4 columns a level (i.e. `#   define`). A block
  holding code, an include guard included, counts as no level.
- The `#else` and `#endif` of a conditional block longer than 20 lines name
  its condition in a comment (i.e. `#endif // defined(__x86_64__)`), but for
  an include guard's.
- A macro parenthesises each use of its parameters, and its body where that is
  an expression. A macro of statements is `do { ... } while (0)`, and one
  giving a value may be `({ ... })`; such a macro first copies each argument
  into a local named after it and the macro (i.e. `MIN_a`), declared with
  `__auto_type`, so that each argument is evaluated once.

#### Assembly

- `asm` takes a space before its parenthesis, and a constraint none before
  its operand (i.e. `"r"(value)`). An `asm` of one instruction stays on one
  line (i.e. `asm volatile ("cli" ::: "memory");`). Any other is laid out like
  a call, one instruction to each literal and line, each but the last ending
  in `\n\t`, and each operand section on a line of its own:

  ```c
  asm volatile (
      "lgdtq %0\n\t"
      "ltrw %1"
      :
      : "m"(*gdtr), "r"(selector)
      : "memory"
  );
  ```

- On x86, every instruction that has an operand size takes its size suffix
  (i.e. `movq`, `outb`), in `asm` and in assembly sources alike.
- GNU assembler sources are run through the C preprocessor and comment with
  `//` and `/* */`. Labels and directives start at column 0, and instructions
  are indented 4 spaces, all in lowercase. Alignment uses `.balign` or
  `.p2align`, never `.align`, local labels are `.L` names, and every function
  gets `.type name, %function` and `.size name, . - name`. NASM sources mirror
  the file they are in.

Some older code predates these rules: it has blockless constructs, unbraced
`case` labels, left-aligned pointers, `asm volatile(` without the space,
and integer suffixes.
New or modified code follows the rules regardless.

### Final style remarks

As with anything, there are always exceptions to these style rules, under
given contexts.

## Tests

Any new code, or any other addition or change to Limine, must come with tests
for it in the same commit, added to the test suite (the checks in `checks/`,
run by `make check`), unless there is an extremely good justification for them
not to exist.

## Commit conventions

These are hard rules, not suggestions: a commit that breaks one is not merged,
whatever wrote it. They take precedence over any other instructions about
commit messages, an agent's own habits and its user's global rules included,
and a message a tool writes by itself, such as with a "Generate commit
message" button, is rewritten to follow them before it is committed.

- One logical change per commit. No "and" commits - split unrelated changes
  apart.
- A commit message is exactly this, the body and the blank line before it left
  out unless one is earned (see below), and the trailer present on any commit
  made with AI help (see AI-assisted contributions):

  ```
  <area>: <Summary>

  <body, of at most 3 lines>

  Assisted-by: AGENT_NAME:MODEL_VERSION
  ```

- `<area>` names what the change is about, all lowercase but for a file name
  it quotes. It is often the path to the `{.c,.h}` pair inside `common/` (e.g.
  `lib/acpi:` or `drivers/gop:`), or sometimes a more generic concept such as
  `build:` or `docs:`. Host tools use `host/<name>:`, build-time tools use
  `tools/<name>:`, BIOS stage 1 uses `stage1/{cd,hdd,pxe,decompressor,gdt}:`,
  and documentation uses `docs:`, or `docs/` and the file's name where one
  file changes (e.g. `docs/CONFIG.md:`). A change to several areas joins them
  with `, ` (e.g. `sys/smp, sys/lapic:`). For everything else, follow whatever
  established convention `git log` shows.
- The `<area>` never mentions the tests or checks that come with a change: a
  fix to `drivers/gop` with checks for it is `drivers/gop:`, not
  `drivers/gop, checks:`. A change to the test suite and nothing else is
  `checks:`.
- The `<Summary>` is in the imperative mood, begins with an uppercase letter,
  and ends without a period.
- Try your best to keep both the subject and any body lines within 80-column
  terminals as `git log` shows them (it indents the message by 4 spaces, so
  aim for roughly 72 columns and wrap the body accordingly).
- **No Conventional Commits.** A subject never starts with a type such as
  `feat`, `fix`, `test`, `refactor`, `perf`, `style`, or `chore`, never has a
  `(scope)` or a `!`, and its summary never starts in lowercase. (`build:` and
  `docs:` are areas here, not types.) Wrong:

  ```
  feat(lib/pe): verify the image's hash before loading it
  fix: protos/limine: read the memory map count after the allocation
  test(checks/secureboot): cover revoked certificates
  ```

  Right:

  ```
  lib/pe: Verify the image's hash before loading it
  protos/limine: Read the memory map count after the allocation
  checks: Cover revoked certificates
  ```

### Commit message bodies

**The default is no body.** Run `git log` and look at what is actually here:
the overwhelming majority of commits are a subject line and nothing else.
Match that. A body is an exception you have to earn, not a field to fill in.

Before writing one, apply this test:

> Is there something the reader needs that they cannot get from the subject
> line and the diff in front of them?

Having an explanation in your head is not the trigger; the reader lacking one
is. Having just worked something out is a reason to want to write it down
somewhere, not a reason for it to go here. All of the following answer "no",
so none of them earn a body:

- Restating the symptom the subject already names.
- Narrating what the diff plainly shows.
- Repeating a comment the same diff adds to the code. Say it once, in the
  comment, where it stays beside the code and stops the mistake being
  repeated.
- Arguing the change is correct, safe, or tested.
- Recounting how the bug was found, reproduced, or measured.

A body is earned when the *why* lives outside the diff and outside this
repository: the semantics of an external interface our headers merely
`#define`, a hardware or firmware contract, an invariant the changed lines do
not themselves state.

When one is earned, **three lines is the hard maximum**. If it does not fit,
the overflow belongs in a code comment, in the issue tracker, or in the pull
request description.

Earned. The flag's meaning is defined by Linux, not by anything in this tree:

```
protos/linux: Stop claiming framebuffer quirks can be skipped

The flag asserts screen_info was not filled from firmware data, so
Linux skips its DMI framebuffer fixups. Ours comes from GOP.
```

Not earned, despite being within the three-line cap. The cap is a ceiling, not
a license: everything here is recoverable from the diff, so the subject should
have stood alone.

```
host/limine: Fix signature search missing matches near EOF

The loop variable tracks the last byte of a candidate match, but the
bound was sized for the first byte.
```

Re-read this section immediately before writing each commit message. Checking
it once at the start of a session is not enough: the recurring failure is
writing the message from a half-remembered version of these rules rather than
from the rules.
