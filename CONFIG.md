# Limine configuration file

## Location of the config file

First, Limine considers configurations supplied to it as SMBIOS OEM String entries (Type 11). Such
configurations are accepted if the first string of such an entry starts with the prefix of
`limine:config:`. The rest of the string is taken as the config file. If such a configuration is
found, no further scanning for config files is done. As such, the `boot():` drive is undefined
on BIOS, and set to the boot device of Limine on UEFI.

Next, Limine scans for a config file on *the boot drive*. Every partition on the boot drive
is scanned sequentially - first partition first (or, on UEFI, the partition containing the
EFI executable of the booted Limine is scanned first), last partition last - for the presence
of either a `/limine.conf`, `/limine/limine.conf`, `/boot/limine.conf`, `/boot/limine/limine.conf`,
or a `/EFI/BOOT/limine.conf` file, in that order.

Once the file is located, Limine will use it as its config file. Other possible
candidates in subsequent partitions or directories are ignored.

It is thus imperative that the intended config file is placed in a location that will
not be shadowed by another candidate config file.

## Structure of the config file

The Limine configuration file is comprised of *menu entries* and *options*.
Comments begin in '#' and can only be on their own lines.

### Menu entries and sub-entries

*Menu entries* describe *entries* which the user can select in the *boot menu*.

A *menu entry* is opened by a line starting with `/` followed by a newline-terminated
string, that being the title of the entry which the user will see.
Any *local option* that comes after it, and before another *menu entry*, or
the end of the file, will be part of that *menu entry*.

A *menu entry* can be a directory, meaning it can hold sub-entries. In order for an
entry to become a directory, it needs to have a sub-entry following right after it.
(A `comment` option may be present between the beginning of the directory entry and
the beginning of the sub-entry).

A *sub-entry* is a menu entry started with a number of `/` greater than 1 prepended to it.
Each `/` represents 1 level deeper down the tree hierarchy of directories and
entries.

Directories can be expanded (meaning they will not show up as collapsed in the
menu) by default if a `+` is put between the `/`s and the beginning of the entry's title.

### Options

*Options* are simple `option_name: string...` style "assignments".
The string can have spaces and other special characters, without requiring quotations. New lines
are delimiters. Option names are not case sensitive.

Some *options* are part of an entry (*local*), some other options are *global*.
*Global options* can appear anywhere in the file and are not part of an entry,
although usually one would put them at the beginning of the config.
Some *local options* work the same between entries using any *protocol*, while other
*local options* are specific to a given *protocol*.

Some options take *paths* as strings; these are described in the next section.

*Global options* are:

Miscellaneous:

* `timeout` - Specifies the timeout in seconds before the first *entry* is automatically booted. If set to `no`, disable automatic boot. If set to `0`, boots default entry instantly (see `default_entry` option).
* `quiet` - If set to `yes`, enable quiet mode, where all screen output except panics and important warnings is suppressed. If `timeout` is not 0, the `timeout` still occurs, and pressing any key during the timeout will reveal the menu and disable quiet mode.
* `serial` - If set to `yes`, enable serial I/O for the bootloader.
* `serial_baudrate` - If `serial` is set to `yes`, this specifies the baudrate to use for serial I/O. Defaults to `9600`. BIOS only, ignored with Limine UEFI.
* `default_entry` - 1-based entry index of the entry which will be automatically selected at startup. If unspecified, it is `1`.
* `remember_last_entry` - If set to `yes`, remember last booted entry. (UEFI only)
* `graphics` - If set to `no`, force CGA text mode for the boot menu, else use a video mode. Ignored with Limine UEFI.
* `wallpaper` - Path to a file to use as a wallpaper. BMP, PNG, and JPEG formats are supported. There can be multiple of this option, in which case the wallpaper will be randomly selected from the provided options.
* `wallpaper_style` - The style which will be used to display the wallpaper image: `tiled`, `centered`, or `stretched`. Default is `stretched`.
* `backdrop` - When the background style is `centered`, this specifies the colour of the backdrop for parts of the screen not covered by the background image, in RRGGBB format.
* `verbose` - If set to `yes`, print additional information during boot. Defaults to not verbose.
* `randomise_memory` - If set to `yes`, randomise the contents of RAM at bootup in order to find bugs related to non zeroed memory or for security reasons. This option will slow down boot time significantly. For the BIOS port of Limine, this will only randomise memory below 4GiB.
* `randomize_memory` - Alias of `randomise_memory`.
* `hash_mismatch_panic` - If set to `no`, do not panic if there is a hash mismatch for a file, but print a warning instead.

Limine interface control options:

* `interface_resolution` - Specify screen resolution to be used by the Limine interface (menu, editor, console...) in the form `<width>x<height>`. This will *only* affect the Limine interface, not any booted OS. If not specified, Limine will pick a resolution automatically. If the resolution is not available, Limine will pick another one automatically. Ignored if using text mode.
* `interface_branding` - A string that will be displayed on top of the Limine interface.
* `interface_branding_colour` - A value between 0 and 7 specifying the colour of the branding string. Default is cyan (6).
* `interface_branding_color` - Alias of `interface_branding_colour`.
* `interface_help_hidden` - Hides the help text located at the top of the screen showing the key bindings.

Limine graphical terminal control options:

These are ignored if using text mode.

* `term_font` - Path to a font file to be used instead of the default one for the menu and terminal. The font file must be a code page 437 character set comprised of 256 consecutive glyph bitmaps. Each glyph's bitmap must be expressed left to right (1 byte per row), and top to bottom (16 bytes per whole glyph by default; see `term_font_size`). See e.g. the [VGA text mode font collection](https://github.com/viler-int10h/vga-text-mode-fonts) for fonts.
* `term_font_size` - The size of the font in dots, which must correspond to the font file or the display will be garbled. Note that glyphs are always one byte wide, and columns over 8 are empty. Many fonts may be used in both 8- and 9-dot wide variants. Defaults to `8x16`. Ignored if `term_font` not set or if the font fails to load.
* `term_font_scale` - Scaling for the font in the x and y directions. `2x2` would display the font in double size, which is useful on high-DPI displays at native resolution. `2x1` only makes the font twice as wide, similar to the VGA 40 column mode. `4x2` might be good for a narrow font on a high resolution display. Values over 8 are disallowed. Default is no scaling, i.e. `1x1`.
* `term_font_spacing` - Horizontal spacing, in pixels, between glyphs on screen. It is equivalent to setting a font width of `<specified width>+<this value>`, except this value is preserved even in case font loading fails, and it also applies to the built-in Limine font. Defaults to 1. 0 is allowed.
* `term_palette` - Specifies the colour palette used by the terminal (RRGGBB). It is a `;` separated array of 8 colours: black, red, green, brown, blue, magenta, cyan, and gray. Ignored if not using a graphical terminal.
* `term_palette_bright` - Specifies the bright colour palette used by the terminal (RRGGBB). It is a `;` separated array of 8 bright colours: dark gray, bright red, bright green, yellow, bright blue, bright magenta, bright cyan, and white. Ignored if not using a graphical terminal.
* `term_background` - Terminal text background colour (TTRRGGBB). TT stands for transparency.
* `term_foreground` - Terminal text foreground colour (RRGGBB).
* `term_background_bright` - Terminal text background bright colour (RRGGBB).
* `term_foreground_bright` - Terminal text foreground bright colour (RRGGBB).
* `term_margin` - Set the amount of margin around the terminal.
* `term_margin_gradient` - Set the thickness in pixel for the gradient around the terminal.

Editor control options:

* `editor_enabled` - If set to `no`, the editor will not be accessible. Defaults to `yes` unless a config hash is enrolled.
* `editor_highlighting` - If set to `no`, syntax highlighting in the editor will be disabled. Defaults to `yes`.
* `editor_validation` - If set to `no`, the editor will not alert you about invalid options or syntax errors. Defaults to `yes`.

*Locally assignable (non protocol specific) options* are:

* `comment` - An optional comment string that will be displayed by the bootloader on the menu when an entry is selected.
* `protocol` - The boot protocol that will be used to boot the kernel/executable. Valid protocols are: `linux`, `limine`, `multiboot` (or `multiboot1`), `multiboot2`, `efi`, `bios`, and `chainload_next`.
* `cmdline` - The command line string to be passed to the kernel/executable. Can be omitted.
* `kernel_cmdline` - Alias of `cmdline`.

Note: `uefi` and `efi_chainload` are aliases of the `efi` protocol option. `bios_chainload` is an alias of the `bios` protocol option.

*Locally assignable (protocol specific) options* are:

* Linux protocol:
  * `path` - The path of the kernel.
  * `kernel_path` - Alias of `path`.
  * `module_path` - The path to a module (such as initramfs). This option can be specified multiple times to specify multiple modules.
  * `resolution` - The resolution to be used. This setting takes the form of `<width>x<height>x<bpp>`. If the resolution is not available, Limine will pick another one automatically. Omitting `<bpp>` will default to 32.
  * `textmode` - If set to `yes`, prefer text mode. (BIOS only)
  * `dtb_path` - A device tree blob to pass instead of the one provided by the firmware.

* Limine protocol:
  * `path` - The path of the executable.
  * `kernel_path` - Alias of `path`.
  * `module_path` - The path to a module. This option can be specified multiple times to specify multiple modules.
  * `module_cmdline` - A command line to be passed to a module. This option can also be specified multiple times. It applies to the module described by the last module option specified.
  * `resolution` - The resolution to be used. This setting takes the form of `<width>x<height>x<bpp>`. If the resolution is not available, Limine will pick another one automatically. Omitting `<bpp>` will default to 32.
  * `kaslr` - For relocatable executables, if set to `no`, disable kernel address space layout randomisation. KASLR is enabled by default.
  * `randomise_hhdm_base` - If set to `yes`, randomise the base address of the higher half direct map. If set to `no`, do not. By default it is `yes` if KASLR is supported and enabled, else it is `no`.
  * `randomize_hhdm_base` - Alias of `randomise_hhdm_base`.
  * `max_paging_mode`, `min_paging_mode` - Limit the maximum and minimum paging modes to one of the following:
    - x86-64 and aarch64: `4level`, `5level`.
    - riscv64: `sv39`, `sv48`, `sv57`.
    - loongarch64: `4level`.
  * `paging_mode` - Equivalent to setting both `max_paging_mode` and `min_paging_mode` to the same value.
  * `dtb_path` - A device tree blob to pass instead of the one provided by the firmware.

* multiboot1 and multiboot2 protocols:
  * `path` - The path of the executable.
  * `kernel_path` - Alias of `path`.
  * `module_path` - The path to a module. This option can be specified multiple times to specify multiple modules.
  * `module_string` - A string to be passed to a module. This option can also be specified multiple times. It applies to the module described by the last module option specified.
  * `resolution` - The resolution to be used should the executable request a graphical framebuffer. This setting takes the form of `<width>x<height>x<bpp>` and *overrides* any resolution requested by the executable. If the resolution is not available, Limine will pick another one automatically. Omitting `<bpp>` will default to 32.
  * `textmode` - If set to `yes`, prefer text mode. (BIOS only)

* EFI Chainload protocol:
  * `path` - Path of the EFI application to chainload.
  * `image_path` - Alias of `path`.
  * `resolution` - The resolution to be used. This setting takes the form of `<width>x<height>x<bpp>`. If the resolution is not available, Limine will pick another one automatically. Omitting `<bpp>` will default to 32.

* BIOS Chainload protocol:
  * `drive` - The 1-based drive to chainload, if omitted, assume boot drive.
  * `partition` - The 1-based partition to chainload, if omitted, or set to 0, chainload drive (MBR).
  * `mbr_id` - Optional. If passed, use an MBR ID (32-bit hex value) to identify the drive containing the volume to chainload. Overrides `drive`, if present, but does *not* override `partition`.
  * `gpt_uuid` or `gpt_guid` - Optional. If passed, use the GPT GUID to identify the drive containing the volume to chainload. Overrides `drive` and `mbr_id`, if present, but does *not* override `partition`.

* chainload_next protocol:
  * `resolution` - For UEFI, the resolution to be used. This setting takes the form of `<width>x<height>x<bpp>`. If the resolution is not available, Limine will pick another one automatically. Omitting `<bpp>` will default to 32.

## Paths

A Limine path is used to locate files in the whole system. It is
comprised of a *resource*, a *resource argument*, and a *path*. It takes the form of:
```
resource(argument):/path
```

The format for `argument` changes depending on the resource used.

A resource can be one of the following:
* `boot` - If booted off PXE this is an alias of `tftp`. Else the `argument` is the 1-based decimal value representing the partition on the boot drive (values of 5+ for MBR logical partitions). If omitted, the partition containing the configuration file on the boot drive is used. For example: `boot(2):/...` will use partition 2 of the boot drive and `boot():/...` will use the partition containing the config file on the boot drive.
* `hdd` - Hard disk drives. The `argument` takes the form of `drive:partition`; for example: `hdd(3:1):/...` would use hard drive 3, partition 1. Partitions and drives are both 1-based (partition values of 5+ for MBR logical partitions). Omitting the partition is possible; for example: `hdd(2:):/...`. Omitting the partition will access the entire volume instead of a specific partition (useful for unpartitioned media).
* `odd` - Optical disk drives (CDs/DVDs/...). The `argument` takes the form of `drive:partition`; for example: `odd(3:1):/...` would use optical drive 3, partition 1. Partitions and drives are both 1-based (partition values of 5+ for MBR logical partitions). Omitting the partition is possible; for example: `odd(2:):/...`. Omitting the partition will access the entire volume instead of a specific partition (useful for unpartitioned media, which is often the case for optical media).
* `guid` - The `argument` takes the form of a GUID/UUID, such as `guid(736b5698-5ae1-4dff-be2c-ef8f44a61c52):/...`. The GUID is that of either a filesystem, when available, or a GPT partition GUID, when using GPT, in a unified namespace.
* `uuid` - Alias of `guid`.
* `fslabel` - The `argument` is the name of the filesystem label of a partition.
* `tftp` - The `argument` is the IP address of the tftp server to load the file from. If the argument is left empty (`tftp():/...`) the file will be loaded from the server Limine booted from. This resource is only available when booting off PXE.

A path can optionally be suffixed with a blake2b hash for the referenced file,
by appending a pound character (`#`) followed by the blake2b hash.
E.g.: `boot():/somemodule.tar#ca6914d2...446b470a`.

## Macros

Macros are strings that can be arbitrarily assigned to represent other strings. For example:
```
${MY_MACRO}=Some text
```

Now, whenever `${MY_MACRO}` is used in the config file (except for an assignment as above), it will
be replaced by the text `Some text`. For example:
```
CMDLINE=something before ${MY_MACRO} something after
```

Macros must always be placed inside `${...}` where `...` is the arbitrary macro name.

### Built-in macros

Limine automatically defines these macros:

* `ARCH` - This built-in macro expands to the architecture of the machine. Possible values are: `x86-64`, `ia-32`, `aarch64`, `riscv64`, `loongarch64`. In the case of IA-32, BIOS or UEFI, the macro will always expand to `x86-64` if the 64-bit extensions are available, else `ia-32`.
* `FW_TYPE` - This built-in macro expands to `UEFI` if booted using UEFI firmware, or `BIOS` if booted using legacy x86 BIOS.
