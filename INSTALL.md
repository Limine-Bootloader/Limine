# Build and Install Instructions

> **NOTE:** This document is about building and installing Limine.
> For information about deployment for usage, see [USAGE.md](USAGE.md).

## Prerequisites

In order to build Limine, the following programs have to be installed:
common UNIX tools (also known as `coreutils`),
`GNU make`, `grep`, `sed`, `find`, `awk`, `gzip`, `nasm`, `mtools`
(optional, necessary to build `limine-uefi-cd.bin`).
Furthermore, `gcc` or `llvm/clang` must also be installed, alongside
the respective binutils.

## Configure

If using a release tarball (recommended, see
https://codeberg.org/Limine/Limine/releases), run `./configure` directly.

If checking out from the repository, run `./bootstrap` first in order to
download the necessary [dependencies](3RDPARTY.md) and generate the configure
script (`GNU autoconf` required).

`./configure` takes arguments and environment variables; for more information
on these, run `./configure --help`.

> **NOTE:** `./configure` by default does not build any Limine port. Make sure
> to read the output of `./configure --help` and enable any or all ports!

Limine supports both in-tree and out-of-tree builds. Simply run the `configure`
script from the directory you wish to execute the build in. The following
`make` commands are supposed to be run inside the build directory.

## Building

To build Limine, run:
```bash
make    # (or gmake where applicable)
```

## Installing

This step will install Limine files to `share`, `include`, and `bin`
directories in the specified prefix (default is `/usr/local`, see
`./configure --help`.

To install Limine, run:
```bash
make install    # (or gmake where applicable)
```
