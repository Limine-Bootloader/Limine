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

If the root directory of the volume Limine was loaded from contains a UEFI
Shell executable, the menu offers a `U` key ("UEFI Shell") that chainloads
it. The file name depends on the Limine EFI executable:

* `shellx64.efi` for `BOOTX64.EFI`
* `shellia32.efi` for `BOOTIA32.EFI`
* `shellaa64.efi` for `BOOTAA64.EFI`
* `shellriscv64.efi` for `BOOTRISCV64.EFI`
* `shellloongarch64.efi` for `BOOTLOONGARCH64.EFI`

This is not available when Limine is booted over PXE.

## Secure Boot
Limine can be booted with Secure Boot if the executable is signed and the key
used to sign it is added to the firmware's keychain (or verified via Shim).
This can be combined with enrolling the BLAKE2b or BLAKE3 hash of the Limine config
file into the Limine EFI executable image itself (`limine enroll-config`).
For config enrollment, BLAKE3 must use 128-byte extended output (256
hexadecimal characters), generated with `b3sum --length 128 limine.conf`,
rather than the default 32-byte output used in resource paths.
For more information see the `limine enroll-config` program and
[the FAQ](FAQ.md).

When Limine detects that UEFI Secure Boot is active (the `SecureBoot` variable
is set and `SetupMode` is not), the following security policies are enforced:

* The config editor is unconditionally disabled, and the UEFI Shell key (`U`) is unavailable.
* Booting from an unmatched boot volume is rejected to prevent hijacking.
* Under Secure Boot, Limine sets `secure_boot = 3` (Enabled) in the Linux `boot_params` setup header and `linux,uefi-secure-boot = <3>` in the Device Tree on RISC architectures, signaling platform lockdown to the operating system.
* **Integrity and Authentication Rules**:
  1. **Kernels and Chainload Images**: Executable images booted via `protocol: linux` or `protocol: efi` must be valid PE/COFF binaries authenticated via Authenticode (by firmware against platform `db`/`dbx` or by Shim via `shim_verify` against MOK). A `#<hash>` is strictly an optional additional pin and does not waive the requirement for an Authenticode-signed PE image. Kernels loaded via protocols that do not perform Authenticode validation (`multiboot1`, `multiboot2`, `limine`) **always** require a `#<hash>` in their URI under Secure Boot, even when the config hash is not enrolled.
  2. **Data Resources (initramfs, modules, DTBs)**: require an associated `#<hash>` **only** when a config checksum has been enrolled via `limine enroll-config`. When the config hash is not enrolled, unhashed modules and initramfs are permitted and measured into TPM PCR 9.
  3. **Wallpapers and Fonts**: without a hash are skipped only when a config hash is enrolled. Under unenrolled Secure Boot, they are loaded with fallback to defaults.
* **Breaking Changes**:
  * **Authenticode Requirement**: Under UEFI Secure Boot, Linux kernels booted via `protocol: linux` must now be PE/COFF binaries signed with a trusted key in UEFI `db` or Shim MOK. Booting an unsigned kernel or a legacy non-PE bzImage without EFI stub (even if hashed in `limine.conf`) is not permitted under Secure Boot and will trigger a verification panic.
  * **Kernel Lockdown**: `secure_boot_active` is no longer cleared when the config is unenrolled. Limine unconditionally sets `secure_boot = 3` in Linux `boot_params` whenever firmware Secure Boot is enabled, triggering kernel lockdown (`CONFIG_LOCK_DOWN_IN_EFI_SECURE_BOOT` / `lockdown=integrity`) on any Secure Boot boot.
* When a config checksum is enrolled via `limine enroll-config`:
  * The config file is verified against the enrolled checksum on every boot; any mismatch causes a panic.
  * All file paths except Authenticode-verified PE kernels and chainload images **must** have a BLAKE2b or BLAKE3 hash appended.
  * `hash_mismatch_panic` is forced to `yes` regardless of the config setting.

When booted under Shim (`shimx64.efi`), Limine integrates with `SHIM_LOCK_PROTOCOL` (using the System V ABI on x86) and hooks `EFI_SECURITY2_ARCH_PROTOCOL`, enabling images signed with MOK keys to be validated via Shim's `shim_verify` hook.

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
artifacts it loads, adhering to the TCG PC Client Platform Firmware Profile and the
[Linux TPM PCR Registry](https://uapi-group.org/specifications/specs/linux_tpm_pcr_registry/):

* **PCR 4**: UEFI firmware automatically measures the Authenticode PE digest of kernel and bootloader binaries loaded via `LoadImage()` (`EV_EFI_BOOT_SERVICES_APPLICATION`). Limine does not duplicate this measurement.
* **PCR 5** receives the on-disk `limine.conf` configuration file bytes, measured as a tagged event (`EV_EVENT_TAG`, event ID `0xf5bc582a` / `LOADER_CONF_EVENT_TAG_ID`, description `limine.conf`).
* **PCR 7** measures platform Secure Boot policies, keys (`PK`, `KEK`, `db`, `dbx`), and the certifying authority (`EV_EFI_VARIABLE_AUTHORITY`, recorded by firmware for platform `db` keys and by Shim for MOK keys).
* **PCR 8** receives, in order, the kernel command line of the booted entry (from its `cmdline` config option, `cmdline: <cmdline>`), the kernel image path (`path: <kernel_path>`), each module/initrd path (`module_path: <module_path>`), and the DTB path (`dtb_path: <dtb_path>`). Any trailing `#<hash>` integrity suffix is stripped before measurement so PCR 8 stays stable across kernel/module/DTB updates; the `$` decompression prefix is preserved as part of the policy.
* **PCR 9** receives the ramdisk / initramfs modules (`module_path: <module_path>`) and device tree blobs (`dtb_path: <dtb_path>` / `efi_dtb`). When booting without Secure Boot, the kernel binary is also measured into PCR 9 (`path: <kernel_path>`).

For backward compatibility, the legacy aliases `TPM_PCR_BOOT_AUTH` (mapping to PCR 8) and `TPM_PCR_LOADED_IMAGES` (mapping to PCR 9) remain available.

All standard measurements use event type `EV_IPL` (`0x0000000d`), while bootloader configuration uses `EV_EVENT_TAG` (`0x00000006`).

On confidential computing platforms (such as AMD SEV-SNP and Intel TDX), Limine simultaneously measures into both `EFI_TCG2_PROTOCOL` and `EFI_CC_MEASUREMENT_PROTOCOL` (translating PCR indices to Memory Reference registers via `MapPcrToMrIndex`). This dual measurement ensures complete attestation coverage across both platform and guest firmware environments.

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

For PCR 4:
* Extended by UEFI firmware (`LoadImage`) with the Authenticode PE digest of the bootloader and kernel binaries.

For PCR 5:
* `limine.conf`: the on-disk `limine.conf` file bytes verbatim (no trailing newline added, no NUL appended), measured with event tag `0xf5bc582a`.

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
* `path: <kernel_path>`: when booting without Secure Boot, the full file bytes of the kernel image as opened by Limine.
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

With BIOS PXE, the TFTP server must support the `tsize` option (RFC 2349):
Limine asks for the size of every file before transferring it, and treats a
file whose size the server does not report as missing.

## UEFI/PXE boot
The `BOOT*.EFI` files are compatible with UEFI PXE.
The steps needed to boot Limine are the same as with BIOS PXE,
except that the `limine-bios.sys` file is not needed on the server.
`tsize` support is needed here too: Limine has the firmware find the size of
every file, which EDK2-based firmware only does through `tsize`.

## Configuration
The `limine.conf` file contains Limine's configuration.

More info on the format of `limine.conf` can be found in
[`CONFIG.md`](CONFIG.md).
