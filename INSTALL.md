# Build and Install Instructions

> **NOTE:** This document is about building and installing Limine.
> For information about deployment for usage, see [USAGE.md](USAGE.md).

## Prerequisites

In order to build Limine, the following programs have to be installed:
common UNIX tools (also known as `coreutils`),
`GNU make`, `grep`, `sed`, `find`, `awk`, `nasm` (necessary to build the x86
ports), `mtools` (optional, necessary to build `limine-uefi-cd.bin`).
Furthermore, `gcc` or `llvm/clang` must also be installed, alongside
the respective binutils.

## Configure

If using a release tarball (recommended, see
https://github.com/Limine-Bootloader/Limine/releases), run `./configure`
directly.

If checking out from the repository, run `./bootstrap` first in order to
download the necessary [dependencies](3RDPARTY.md) and generate the configure
script (`git`, `patch`, `GNU autoconf` 2.64 or later and `GNU automake` 1.13
or later required).

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

## Testing

Limine comes with a test suite that boots it under QEMU on each port the build
enabled. QEMU runs without a window, so the suite runs headless, from a text
terminal or over SSH, and it needs no root privileges.

### Test suite prerequisites

Besides what building Limine needs, the test suite needs:
- `python3`;
- QEMU 9.2 or later, though before 10.0 the TPM tests skip loongarch64:
  `qemu-system-x86_64` for the BIOS, IA-32 UEFI, and x86-64 UEFI ports, and
  `qemu-system-aarch64`, `qemu-system-riscv64`, and `qemu-system-loongarch64`
  for the other UEFI ports;
- `mtools`, `mkfs.fat` (from `dosfstools`), and `xorriso`, which make the disk
  and CD images;
- `swtpm`, for the TPM tests; `openssl` and `virt-fw-vars` (from
  `virt-firmware`), for the UEFI variable stores and Secure Boot keys; and
  `b3sum`, for the BLAKE3 hash tests;
- `curl`, `gunzip`, and `tar`, to download and unpack the UEFI firmware;
- `ps`, to find the `-j` option `make check` was run with;
- for coverage reports, `clang`, `llvm-objdump`, `llvm-symbolizer`,
  `llvm-profdata`, and `llvm-cov`.

The kernels and other programs the tests boot are built with the toolchain
that builds Limine.

### Running the tests

To run the test suite, run:
```bash
make check    # (or gmake where applicable)
```

This builds Limine and the programs the tests boot, and runs every test on
every port the build enabled, as many at a time as `make`'s `-j` option asks
for, as in `make check -j8`, or one at a time without it. The UEFI ports
boot the firmware from https://github.com/osdev0/edk2-ovmf-stable-bins, which
the first `make check` downloads into the `edk2-ovmf-bins` directory; delete
that directory to have the next one download the latest release.

A line is printed for each test as it finishes. The results go to the
`checks-results` directory: a directory for each test and port, with its logs,
screenshots, and, if it failed, its disk images, and a `results.json` file
summing up the whole run.

Options for the test runner go in `TESTFLAGS`, as in
`make check TESTFLAGS='-p bios -t menu'`:
- `-k PATTERN`: only run the tests whose name matches the glob `PATTERN`, or
  contains it.
- `-p PORT`: only run tests on `PORT`, one of `bios`, `uefi-ia32`,
  `uefi-x86-64`, `uefi-aarch64`, `uefi-riscv64`, and `uefi-loongarch64`.
- `-t TAG`: only run the tests tagged `TAG`, such as `menu`, `linux`, or `tpm`.
- `-j JOBS`: run `JOBS` tests at a time, whatever `make`'s own `-j` says. Run
  directly, the test runner defaults to half the number of CPUs, up to 8.
- `--list`: list the tests that would run instead of running them.
- `--keep`: keep the disk images of the tests that passed too.
- `--outdir DIR`: put the results in `DIR`.
- `--ovmfdir DIR`: take the UEFI firmware from `DIR`.
- `--coverage`: record which lines of Limine and of the `limine` host tool the
  tests run, and report it in the `coverage` directory among the results.

`-k`, `-p`, and `-t` can each be given more than once, and a test runs on a
port when it matches one of the values given for each of them. The test runner
is `checks/run-tests` in the source tree, and `checks/run-tests --help` lists
all of its options. On a slow machine, setting the `LIMINE_TEST_SLOWDOWN`
environment variable to a number multiplies every timeout by it.

To remove the test programs and results, run:
```bash
make check-clean    # (or gmake where applicable)
```

`make clean` removes them as well.

## Installing

This step will install Limine files to `share` and `bin` directories in the
specified prefix (default is `/usr/local`, see `./configure --help`).

To install Limine, run:
```bash
make install    # (or gmake where applicable)
```
