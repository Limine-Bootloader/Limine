# Usage

> **NOTE:** The Limine files referred to here are those contained inside
> ${PREFIX}/share/, installed there as a product of the steps described in
> [INSTALL.md](INSTALL.md).

## UEFI
The `BOOT*.EFI` files are valid EFI applications that can be simply copied to
the `/EFI/BOOT` directory of a FAT formatted EFI system partition. These files
can be installed there and coexist with a BIOS installation of Limine
(see below) so that the disk will be bootable on both BIOS and UEFI systems.

A valid config file should also be provided as described in
[CONFIG.md](CONFIG.md).

## Secure Boot
Limine can be booted with Secure Boot if the executable is signed and the key
used to sign it is added to the firmware's keychain. This should be done in
combination with enrolling the BLAKE2B hash of the Limine config file into the
Limine EFI executable image itself for verification purposes.
For more information see the `limine enroll-config` program and
[the FAQ](FAQ.md).

When Limine detects that UEFI Secure Boot is active (the `SecureBoot` variable
is set and `SetupMode` is not) **and** a config BLAKE2B checksum is enrolled
in the Limine EFI executable, the following security policies are enforced:

* The config file is verified against the enrolled checksum on every boot.
  Any mismatch will cause a panic.
* All file paths (kernels, modules, DTBs, fonts, etc.) **must** have a BLAKE2B
  hash appended (e.g. `boot():/kernel#<hash>`). Loading a file without a hash
  will cause a panic. The exception is EFI chainloading, where the firmware's
  own Secure Boot image verification is used instead.
* Wallpaper and font files without an associated hash are silently skipped
  (falling back to defaults) rather than causing a panic.
* The config editor is unconditionally disabled.
* `hash_mismatch_panic` is forced to `yes` regardless of the config setting.

If no config checksum is enrolled, Limine treats Secure Boot as inactive and
none of the above hardening is applied. Enrolling a checksum is the explicit
opt-in to Secure Boot enforcement; an unenrolled image can still be signed
and booted under Secure Boot, but it provides no integrity guarantees beyond
those of the firmware itself.

## BIOS/MBR
In order to install Limine on a MBR device (which can just be a raw image
file), run `limine bios-install` as such:

```bash
limine bios-install <path to device/image>
```

The boot device must contain the `limine-bios.sys` and `limine.conf` files in
either a `boot/limine`, `boot`, `limine`, or root directory of one of the
partitions, formatted with a supported file system. See [CONFIG.md](CONFIG.md).

## BIOS/GPT
If using a GPT formatted device, create a partition on the GPT device (usually
of "BIOS boot" type) of at least 32KiB in size, and pass the 1-based number
of the partition to `limine bios-install` as a second argument; such as:

```bash
limine bios-install <path to device/image> <1-based stage 2 partition number>
```

The boot device must contain the `limine-bios.sys` and `limine.conf` files in
either a `boot/limine`, `boot`, `limine`, or root directory of one of the
partitions, formatted with a supported file system. See [CONFIG.md](CONFIG.md).

## BIOS/UEFI hybrid ISO creation
In order to create a hybrid ISO with Limine, place the
`limine-uefi-cd.bin`, `limine-bios-cd.bin`, `limine-bios.sys`, and
`limine.conf` files into a directory which will serve as the root of the
created ISO.
(`limine-bios.sys` and `limine.conf` must either be in the root, `limine`,
`boot`, or `boot/limine` directory; `limine-uefi-cd.bin` and
`limine-bios-cd.bin` can reside anywhere).

After that, create a `<ISO root directory>/EFI/BOOT` directory and copy the
relevant Limine EFI executables over (such as `BOOTX64.EFI`).

Place any other file you want to be on the final ISO in said directory, then
run:
```
xorriso -as mkisofs -R -r -J -b <relative path of limine-bios-cd.bin> \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot <relative path of limine-uefi-cd.bin> \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        <root directory> -o image.iso
```

*Note: `xorriso` is required.*

And do not forget to also run `limine bios-install` on the generated image:
```
limine bios-install image.iso
```

`<relative path of limine-bios-cd.bin>` is the relative path of
`limine-bios-cd.bin` inside the root directory.
For example, if it was copied in `<root directory>/boot/limine-bios-cd.bin`,
it would be `boot/limine-bios-cd.bin`.

`<relative path of limine-uefi-cd.bin>` is the relative path of
`limine-uefi-cd.bin` inside the root directory.
For example, if it was copied in
`<root directory>/boot/limine-uefi-cd.bin`, it would be
`boot/limine-uefi-cd.bin`.

## BIOS/PXE boot
The `limine-bios-pxe.bin` binary is a valid PXE boot image.
In order to boot Limine from PXE it is necessary to setup a DHCP server with
support for PXE booting. This can either be accomplished using a single DHCP
server or your existing DHCP server and a proxy DHCP server such as dnsmasq.

`limine.conf` and `limine-bios.sys` are expected to be on the server used for
boot.

## UEFI/PXE boot
The `BOOT*.EFI` files are compatible with UEFI PXE.
The steps needed to boot Limine are the same as with BIOS PXE,
except that the `limine-bios.sys` file is not needed on the server.

## Configuration
The `limine.conf` file contains Limine's configuration.

More info on the format of `limine.conf` can be found in
[`CONFIG.md`](CONFIG.md).
