# Contributing to Limine

Thanks for contributing. A few requirements apply before a change can be merged.

## Sign-off (DCO)

Every commit must be signed off by a human contributor:

```
git commit -s
```

This adds a `Signed-off-by: Your Name <your@email>` trailer certifying that you wrote the
change (or otherwise have the right to submit it) and agree to contribute it under the
project's license, per the Developer Certificate of Origin (the [`DCO`](DCO) file in this
repository; also at https://developercertificate.org).
The sign-off is always a human's, and is never added by a tool on behalf of a human.

## Style and commit conventions

See [AGENTS.md](AGENTS.md) for code style and commit-message rules (single-line
`area: summary` subjects, one logical change per commit, ASCII only, and so on). These apply
to every contribution, AI-assisted or not.

## AI-assisted contributions

AI-assisted contributions are welcome, subject to the following:

- **A human is in the loop at all times.** AI tools may assist, but a human contributor must
  drive the work, review and understand every generated change, and take full responsibility
  for it. Do not submit code you have not read and understood.
- **The assistance must be disclosed.** Any commit produced with material AI help *must*
  carry an `Assisted-by:` trailer naming the model, for example:

  ```
  Assisted-by: Claude:claude-opus-4-8
  ```

- The human contributor still signs off (see above). `Signed-off-by:` is the human's
  certification of, and responsibility for, the change; `Assisted-by:` only records which
  tool helped. It does not replace the sign-off or the human review.

Unreviewed, bulk, or fully-automated submissions are not accepted.
