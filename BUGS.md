# Bugs

The following issues are known to us and may require a firmware fix.

## Limine freezes for 4-5 seconds after first keypress

Known affected hardware: ThinkPad X1 Carbon Gen 13, 8 × Intel® Core™ Ultra 7 268V

Issue: When pressing any key during the countdown, Limine freezes for around 4-5 seconds before responding to the keypress. This happens regardless of whether the key pressed is enter or an arrow key to navigate through the menu.

Resolution: The firmware present on some machines lazily initializes the keyboard stack on the very first input read. This procedure takes around 4-5 seconds, varing between computers. The issue can not be fixed in Limine, and the only known workaround might be to update the firmware to a version that does not have this issue. The problem reproduces on different bootloaders, such as systemd-boot.

Comment: Sometimes the initialisation of the keyboard stack non-deterministically fails entirely, perhaps due to Limine's use of `WaitForEvent` rather than polling. Both features should be supported by the firmware correctly.

Ticket(s): #606, #563

## Hard reboot at ExitBootServices

Known affected hardware: ASUS TUF A15 FA507NU (BIOS version 318, tried with 316 but it also wasn't working); HP model: 887C v: 59.25, Firmware: UEFI vendor: AMI v: F.35 date: 10/23/2024; Z490 AORUS ELITE AC and the BIOS was from 2021. Primarily affects ASUS and HP motherboards.
Known unaffected hardware: ASUSTeK COMPUTER INC. ASUS TUF Gaming A15 FA506NC_FA506NC/FA506NC, BIOS FA506NC.305 02/22/2024; Z490 AORUS ELITE AC with most recent BIOS as of 2026.

Issue: When Limine attempts to hand over control to the booted operating system, the computer hard reboots instead of booting the OS. This issue is only present on some computers, and it is not clear what causes it.

Resolution: Multiple workarounds exist; among them is starting Limine from the UEFI setup menu manually, using EFI fallback, upgrading firmware, using a version prior to v12.5.0.

Comment: We believe that there is a chance that the issue can be fixed from within Limine (in form of a workaround) or may originate from an issue in Limine itself, but we can not reproduce the bug on any of the five diverse machines we had access to and we have not been able to find a root cause for the issue. We are open to any contributions that may help us fix this issue.

Ticket(s): #610