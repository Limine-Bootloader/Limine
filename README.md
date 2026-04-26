# Limine [![Matrix Server](https://img.shields.io/matrix/limine:matrix.org?color=000000&label=Matrix&logo=matrix)](https://matrix.to/#/#limine:matrix.org)

<p align="center">
    <img src="https://github.com/Limine-Bootloader/Limine/blob/trunk/logo.png?raw=true" alt="Limine's logo"/>
</p>

### What is Limine?

Limine (pronounced as demonstrated [here](https://www.merriam-webster.com/dictionary/in%20limine))
is a modern, advanced, portable, multiprotocol bootloader and boot manager, also used
as the reference implementation for the [Limine boot protocol](https://github.com/Limine-Bootloader/limine-protocol/blob/trunk/PROTOCOL.md).

### Community, Support, and Donations

#### Donate
If you want to support the work I ([@mintsuki](https://github.com/Mintsuki)) do on Limine, feel free to donate to me on Liberapay:
<p><a href="https://liberapay.com/mintsuki/donate"><img alt="Donate using Liberapay" src="https://liberapay.com/assets/widgets/donate.svg"></a></p>

Donations welcome, but absolutely not mandatory!

#### Community
We have a Matrix room at [`#limine:matrix.org`](https://matrix.to/#/#limine:matrix.org) if you need support, info, or you just want to hang out with us.

### Limine's boot menu

![Reference screenshot](screenshot.png?raw=true "Reference screenshot")

[Photo by Levent Simsek](https://www.pexels.com/photo/brown-tabby-cat-in-close-up-photography-3617160/)

### Supported architectures
* IA-32 (32-bit x86)
* x86-64
* aarch64 (arm64)
* riscv64
* loongarch64

### Supported boot protocols
* Linux
* [Limine](https://github.com/Limine-Bootloader/limine-protocol/blob/trunk/PROTOCOL.md)
* Multiboot 1
* Multiboot 2
* Chainloading

### Supported partitioning schemes
* MBR
* GPT
* Unpartitioned media

### Supported filesystems
* FAT12/16/32
* ISO9660 (CDs/DVDs)

If your filesystem isn't listed here, please read [the FAQ](FAQ.md) first, especially before
opening issues or pull requests related to this.

### Minimum system requirements
For 32-bit x86 systems, support is only ensured starting with those with
Pentium Pro (i686) class CPUs.

All x86-64, aarch64, riscv64 and loongarch64 (UEFI) systems are supported.

## Packaging status

All Limine releases since 7.x use [Semantic Versioning](https://semver.org/spec/v2.0.0.html) for their naming.

[![Packaging status](https://repology.org/badge/vertical-allrepos/limine.svg?columns=3)](https://repology.org/project/limine/versions)

## Binary releases

For convenience, for point releases, binaries are distributed. These binaries
are shipped as assets as part of the
[Limine GitHub releases](https://github.com/Limine-Bootloader/Limine/releases)
(see the `limine-binary-*` files).

The `limine` host tool is shipped in highly portable source form as part of the
binary release package. For most/all UNIX-like OSes, in order to rebuild it,
simply run `make` in the unpacked binary release directory. Alternatively, it
can be built stand-alone using any C99 compatible compiler.

`limine` host tool binaries for x86 Windows are provided as part of the binary
release package.

## Build and Install Instructions

*The following steps are not necessary if using a binary release.*

See [INSTALL.md](INSTALL.md).

## Usage

See [USAGE.md](USAGE.md).

## 3rd Party Software Acknowledgments

See [3RDPARTY.md](3RDPARTY.md).
