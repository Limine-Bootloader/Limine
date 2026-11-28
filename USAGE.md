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

This is not available when Limine is booted over PXE or UEFI Secure Boot is
active.

## Secure Boot
Limine enforces authenticated boot when `SecureBoot` is `1` and `SetupMode`
is `0` or absent. If only `SetupMode` is absent, a valid `SecureBoot` value
determines whether enforcement is active. If both variables are absent, Secure
Boot is unavailable. Present `SecureBoot`, `SetupMode`, `AuditMode`, and
`DeployedMode` variables must each be a one-byte boolean with boot-service and
runtime access; extra attributes are accepted. `AuditMode` cannot be `1` while
`SecureBoot` is `1` or while `SetupMode` is `0`, and `DeployedMode` cannot be
`1` while `SetupMode` or `AuditMode` is `1`. Read errors, a lone `SetupMode`,
or inconsistent values halt the boot.

While enforcement is active:

* Linux (`protocol: linux`) and chainloaded (`protocol: efi`) entries are
  started through firmware `LoadImage` and `StartImage` so the firmware
  verifies their PE/COFF signature against `db`/`dbx`.
* For `protocol: linux`, repeated `module_path` entries supply initramfs
  archives in order via `EFI_LOAD_FILE2_PROTOCOL`.
* Limine halts if it cannot match its boot device handle; it does not prompt
  or search unrelated drives for a configuration. PXE boot and `tftp` URIs are
  unavailable. For an on-disk `limine.conf`, EFI images, and external Linux
  initramfs archives, it opens regular files through
  `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` (including an El Torito EFI system
  partition on optical media). It skips candidate config volumes without this
  protocol and halts if no configuration file is found (unless an SMBIOS Type
  11 `limine:config:` entry is supplied). There is no fallback to native FAT or
  ISO 9660 readers, or to TFTP, for these files. A short config read is fatal.
  `$` URI decompression is unavailable for EFI images and external initramfs.
* Both `protocol: linux` and `protocol: efi` (and its aliases `uefi` and
  `efi_chainload`) reject `module_string` and `module_cmdline`, the `initrd`,
  `initrdmem`, and `dtb` command-line options, arguments beginning with
  `noinitrd`, `initrd=`, `initrdmem=`, and `dtb=` substrings, and ASCII
  controls below space other than tab, and halt after cleanup if the child
  image returns. `protocol: linux` also rejects external DTBs (`dtb_path` and
  `global_dtb`). Enrolling a config hash keeps `protocol: linux` on this
  firmware-verified EFI image path while requiring `#<hash>` on `module_path`.
* The config editor and UEFI Shell hotkey (`U`) are disabled, and
  `hash_mismatch_panic` is forced to `yes`.
* External wallpapers and fonts are skipped unless a config checksum is
  enrolled and their paths carry matching hashes.

A signed EFI-stub kernel with an external initramfs is configured as usual:

```ini
/Linux
    protocol: linux
    path: boot():/vmlinuz-linux
    module_path: boot():/initramfs-linux.img
    cmdline: root=UUID=example rw
```

Enrolling a config hash into the Limine binary (before signing it) is optional:

* **Without an enrolled config hash:** `limine.conf` can be edited without
  re-signing the Limine executable. Only `linux`, `efi` (and its aliases),
  and `efi_boot_entry` are available. Kernels and EFI applications are verified
  by firmware `LoadImage`. Limine measures `limine.conf` and a non-empty
  supplied command line only when the TPM 2.0 capabilities below are
  available. The Linux EFI stub measures external initramfs archives when
  supported. This measurement does not authenticate an external archive
  against `db`. To bind the initramfs to an EFI image signature, embed it in
  a signed UKI and omit external `module_path` entries.
* **With an enrolled config hash:** `limine.conf` is verified against the
  embedded BLAKE2b or 128-byte BLAKE3 (`b3sum --length 128 limine.conf`)
  digest on boot, and all non-EFI-image paths (such as `module_path` initramfs
  files) **must** carry a `#<hash>` suffix. `limine` is also available, as
  are `multiboot` (or `multiboot1`) and `multiboot2` on x86.
  Their executables, modules, and any external DTB they use are authenticated
  by the hashes in the enrolled config, not by firmware image verification.
  See `limine enroll-config` and [the FAQ](FAQ.md).

If a TPM is present, `EFI_TCG2_PROTOCOL` must report structure and protocol
versions 1.1 or later, the TCG2 event log format, and at least one active
SHA-256, SHA-384, SHA-512, SHA3-256, SHA3-384, or SHA3-512 PCR bank;
`measured_boot: no` does not disable it.

If no TPM is present, Limine warns once and boots without its own
measurements. Discovery, capability, or extend errors on a present TPM 2.0
interface halt the boot. The exception is `EFI_VOLUME_FULL`: the PCR was
extended but the log missed the event. Limine warns once and continues
without retrying. It leaves retrieval of the event log to the EFI child; a
verifier should not accept a replay that disagrees with the PCRs.

On the EFI image path, the PCRs have distinct owners:

* **PCR 4:** Firmware measures EFI executable images during `LoadImage` as
  `EV_EFI_BOOT_SERVICES_APPLICATION`.
* **PCR 7:** Firmware measures Secure Boot policy and signing authorities.
* **PCR 9:** Limine measures the raw `limine.conf` bytes as `EV_IPL`, with the
  NUL-terminated description `limine_cfg`. Later, a Linux EFI stub that
  supports measuring the external initramfs measures the buffer it obtains
  through `EFI_LOAD_FILE2_PROTOCOL`.
* **PCR 12:** After `LoadImage` succeeds and before `StartImage`, Limine
  measures non-empty `LoadOptions` as `EV_IPL`: the UTF-16LE bytes including
  the terminating UTF-16 NUL. Empty or omitted options leave `LoadOptions`
  `NULL` and `LoadOptionsSize` zero. Neither they nor a rejected image cause
  a Limine PCR 12 event. A UKI may use an embedded command line instead; the
  event describes the options supplied, not necessarily those used.

Limine does not extend PCR 5. Firmware can still extend it.

## Measured Boot
Under UEFI Secure Boot, measured boot is mandatory when the firmware exposes
`EFI_TCG2_PROTOCOL` with a TPM present. `measured_boot: no` cannot disable
it. Outside Secure Boot, measured boot is opt-in: set `measured_boot: yes`
to enable it. Otherwise Limine extends no PCRs, including PCR 9 for the
configuration. Native handoff still passes on the firmware's pre-boot event
log when one is available.

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
* **PCR 9** receives, in order, the raw `limine.conf` bytes, the native
  kernel image as loaded, each module/initrd in config order, and, when the
  booted protocol consumes a device tree blob, the DTB as loaded (taken from
  `dtb_path`/`global_dtb` if set, otherwise from the firmware's
  `EFI_DTB_TABLE_GUID` table) before Limine's `/chosen` and memory-node
  fixups.

These PCR 8 and PCR 9 measurements use event type `EV_IPL` (`0x0000000d`).
Each event's payload in the TCG log is the NUL-terminated description
identifying what was measured: `cmdline: <cmdline>` for command lines,
`path: <kernel_path>` for the kernel image, `module_path: <module_path>` for
modules and initrds, `dtb_path: <dtb_path>` for a DTB loaded from a file,
`efi_dtb` for the firmware-provided DTB, and `limine_cfg` for the
`limine.conf` blob.

Native Linux outside Secure Boot publishes the captured TCG event log via the
`LINUX_EFI_TPM_EVENT_LOG` configuration table. The Limine boot protocol can
provide the log through its TPM Event Log feature, including when the config
is enrolled under Secure Boot. Under UEFI Secure Boot, `protocol: linux` boots
through firmware `LoadImage` and does not publish `LINUX_EFI_TPM_EVENT_LOG`.
Native handoff can supply a truncated firmware log as a best-effort record;
the presence of a log is not proof that it can reproduce the PCRs.

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

* `limine_cfg` (`EV_IPL`): hash the configuration bytes verbatim, before
  parsing. For a disk file, do not add a newline or a NUL. The description's
  terminating NUL is not part of the digest. With a TPM present, this event
  is mandatory under UEFI Secure Boot; outside Secure Boot, it is emitted
  only with `measured_boot: yes`.
* `path: <kernel_path>`: the full file bytes of the kernel image as opened
  by Limine (post-decompression for `$`-prefixed paths, after hash
  verification when Secure Boot is active).
* `module_path: <module_path>`: the full file bytes of the module/initrd as
  loaded.
* `dtb_path: <dtb_path>`/`efi_dtb`: the device tree blob bytes as loaded,
  before Limine's `/chosen` and memory-node fixups (i.e. exactly the bytes
  on disk or in the firmware's `EFI_DTB_TABLE_GUID` table).

For PCR 12 on the authenticated EFI image path, encode the supplied options
as UTF-16LE without a byte-order mark, append two zero bytes, and hash that
buffer. Do not extend for an empty or omitted command line.

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
`limine-bios-cd.bin` can reside anywhere.)

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
