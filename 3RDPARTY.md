# 3rd Party Software Acknowledgments

The Limine project depends on several other projects.

(For readers with access to source code, know that these are pulled in by the
`./bootstrap` script, or, in the case of release tarballs, are shipped
alongside the core Limine code in the tarballs themselves, similar to
`./bootstrap` having already been run.)

These additional projects are NOT covered by the License as contained inside
the `COPYING` file as present at the root of the source tree, or, for installed
copies, present at `${DOCDIR}/COPYING` (assuming the file has not been
otherwise removed by the packager). These are instead licensed as described by
each individual project's documentation present in each project's dedicated
subdirectory or license header(s) in the source tree. For readers without access
to the source code, one can read the following for a quick overview of licenses
that Limine is distributed under:

A non-binding, informal summary of all projects Limine depends on, and the
licenses used by said projects, in SPDX format, is as follows:

- [cc-runtime](https://codeberg.org/OSDev/cc-runtime)
(Apache-2.0 WITH LLVM-exception) is used to provide runtime libgcc-like
routines.

- [0BSD Freestanding C Headers](https://codeberg.org/OSDev/freestnd-c-hdrs-0bsd)
(0BSD) provide GCC and Clang compatible freestanding C headers.

- [Limine Boot Protocol](https://codeberg.org/Limine/limine-protocol)
(0BSD) has the C/C++ header and the specification text of the Limine Boot
Protocol.

- [PicoEFI](https://codeberg.org/PicoEFI/PicoEFI) (multiple licenses, see list
below) provides headers and build-time support for UEFI.
    - BSD-2-Clause
    - BSD-2-Clause-Patent
    - BSD-3-Clause
    - LicenseRef-scancode-bsd-no-disclaimer-unmodified
    - MIT

    For more information about the
    LicenseRef-scancode-bsd-no-disclaimer-unmodified license used by parts of
    PicoEFI, see
    https://scancode-licensedb.aboutcode.org/bsd-no-disclaimer-unmodified.html
    and the LicenseRef file
    [here](LICENSES/LicenseRef-scancode-bsd-no-disclaimer-unmodified.txt),
    in case of viewing this file from inside the source tree, alternatively at
    `${DOCDIR}/LICENSES/LicenseRef-scancode-bsd-no-disclaimer-unmodified.txt`
    in case of installed copies, assuming the file has not been otherwise
    removed by the packager.

- [tinf](https://github.com/jibsen/tinf) (Zlib) is used in early x86 BIOS
stages for GZIP decompression of stage2.

- [Flanterm](https://codeberg.org/Mintsuki/Flanterm) (BSD-2-Clause) is used for
text related screen drawing.

- [stb_image](https://github.com/nothings/stb/blob/master/stb_image.h) (MIT) is
used for wallpaper image loading.

- [libfdt](https://codeberg.org/OSDev/libfdt) (BSD-2-Clause) is used for
manipulating Flat Device Trees.

Note that some of these projects, or parts of them, are provided under
dual-licensing, in which case, in the above list, the only license mentioned is
the one chosen by the Limine developers. Refer to each individual project's
documentation for details.
