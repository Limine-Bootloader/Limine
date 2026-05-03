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

## Measured Boot
Measured boot is opt-in. Limine performs measurements only when the
`measured_boot` config option is set to `yes` (also forced on under UEFI
Secure Boot) **and** the firmware exposes `EFI_TCG2_PROTOCOL` (or
`EFI_CC_MEASUREMENT_PROTOCOL` on confidential computing platforms such as
Intel TDX and AMD SEV-SNP). With either condition unmet, no PCR is extended
by the bootloader; firmware's pre-boot event log is still captured and
relayed if a TPM is present, since it carries useful PCR 0-7 information
regardless of what Limine does.

When measured boot is active, Limine extends the platform PCRs with the
artifacts it loads, following the GRUB convention from the
[UAPI Linux TPM PCR Registry](https://uapi-group.org/specifications/specs/linux_tpm_pcr_registry/):

* **PCR 8** receives, in order, the kernel command line of the booted entry
  (from its `cmdline` config option, exactly as Limine will hand it to the
  kernel), the kernel image's path, each module/initrd's path in the order
  they appear in the config, and the DTB's path when the booted protocol
  consumes one specified via `dtb_path` or `global_dtb`. Any trailing
  `#<hash>` integrity suffix is stripped before measurement so PCR 8 stays
  stable across kernel/module/DTB updates; the `$` decompression prefix is
  preserved as part of the policy.
* **PCR 9** receives, in order, the on-disk `limine.conf` bytes (before any
  in-memory cleanup), the kernel image as read from disk, each module/initrd
  in the order they appear in the config, and, when the booted protocol
  consumes a device tree blob, the DTB as loaded (taken from
  `dtb_path`/`global_dtb` if set, otherwise from the firmware's
  `EFI_DTB_TABLE_GUID` table) before Limine's `/chosen` and memory-node
  fixups.

All measurements use event type `EV_IPL` (`0x0000000d`). Each event's payload
in the TCG log is the NUL-terminated description identifying what was
measured: `cmdline: <cmdline>` for command lines, `path: <kernel_path>` for
the kernel image, `module_path: <module_path>` for modules and initrds,
`dtb_path: <dtb_path>` for a DTB loaded from a file, `efi_dtb` for the
firmware-provided DTB, and `limine_cfg` for the `limine.conf` blob.

On confidential computing platforms each PCR index is translated to the
corresponding Memory Reference (MR) register via
`EFI_CC_MEASUREMENT_PROTOCOL.MapPcrToMrIndex`; the rest of the contract is
unchanged.

The captured TCG event log is published to the operating system via the
`LINUX_EFI_TPM_EVENT_LOG` configuration table (for the Linux protocol), or
via the TPM Event Log feature (for the Limine boot protocol).

The following additional behaviours also apply, so that the PCR state at
handoff is consistent across attempts:

* Any panic halts the system unconditionally; there is no return to the menu,
  so a partially-extended PCR chain cannot be re-extended on a second attempt.
* On the IA-32 UEFI port, modules **must** fit below 4 GiB. Firmware's
  `HashLogExtendEvent` cannot reach addresses above 4 GiB on a 32-bit
  firmware, so an above-4-GiB allocation would result in an unmeasured module.

### Reproducing the digests
For an external verifier to recompute a PCR extend, hash exactly these bytes:

For PCR 8:

* `cmdline: <cmdline>`: the kernel command line bytes, without trailing
  NUL, i.e. `strlen(cmdline)` bytes.
* `path: <kernel_path>`: the kernel image's path string with any trailing
  `#<hash>` suffix stripped, without trailing NUL.
* `module_path: <module_path>`: the module/initrd's path string with any
  trailing `#<hash>` suffix stripped, without trailing NUL.
* `dtb_path: <dtb_path>`: the DTB file's path string with any trailing
  `#<hash>` suffix stripped, without trailing NUL.

For PCR 9:

* `limine_cfg`: the on-disk `limine.conf` file bytes verbatim (no trailing
  newline added, no NUL appended).
* `path: <kernel_path>`: the full file bytes of the kernel image as opened
  by Limine (post-decompression for `$`-prefixed paths, after BLAKE2B hash
  verification when Secure Boot is active).
* `module_path: <module_path>`: the full file bytes of the module/initrd as
  loaded.
* `dtb_path: <dtb_path>`/`efi_dtb`: the device tree blob bytes as loaded,
  before Limine's `/chosen` and memory-node fixups (i.e. exactly the bytes
  on disk or in the firmware's `EFI_DTB_TABLE_GUID` table).

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
