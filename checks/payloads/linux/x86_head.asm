; A bzImage real-mode header (Linux Documentation/arch/x86/boot.rst) in front
; of the protected-mode payload. Options come in as LOADFLAGS, XLOADFLAGS,
; RELOCATABLE, PREF_ADDRESS, VERSION and INITRD_ADDR_MAX.

bits 16
    times 0x1f1 - ($ - $$) db 0
setup_sects:        db 1
root_flags:         dw 0
syssize:            dd (pm_end - pm_start + 15) / 16
ram_size:           dw 0
vid_mode:           dw 0
root_dev:           dw 0
boot_flag:          dw 0xaa55
jump:               db 0xeb, header_end - 0x202
header:             db 'HdrS'
version:            dw VERSION
realmode_swtch:     dd 0
start_sys_seg:      dw 0x1000
kernel_version:     dw kernel_version_str - 0x200
type_of_loader:     db 0
loadflags:          db LOADFLAGS
setup_move_size:    dw 0x8000
code32_start:       dd 0x100000
ramdisk_image:      dd 0
ramdisk_size:       dd 0
bootsect_kludge:    dd 0
heap_end_ptr:       dw 0
ext_loader_ver:     db 0
ext_loader_type:    db 0
cmd_line_ptr:       dd 0
initrd_addr_max:    dd INITRD_ADDR_MAX
kernel_alignment:   dd 0x200000
relocatable_kernel: db RELOCATABLE
min_alignment:      db 21
xloadflags:         dw XLOADFLAGS
cmdline_size:       dd 2047
hardware_subarch:   dd 0
hardware_subarch_data: dq 0
payload_offset:     dd 0
payload_length:     dd 0
setup_data:         dq 0
pref_address:       dq PREF_ADDRESS
init_size:          dd 0x40000
handover_offset:    dd 0
kernel_info_offset: dd 0
header_end:

kernel_version_str: db '6.12.0-limine-test (test@limine) #1 SMP', 0

    times 1024 - ($ - $$) db 0
pm_start:
    incbin "pm.bin"
pm_end:
