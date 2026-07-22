# Bugs

The following issues are known to us and may require a firmware fix.

## Limine freezes for 4-5 seconds after first keypress

Known affected hardware: ThinkPad X1 Carbon Gen 13, 8 x Intel Core Ultra 7 268V

Issue: When pressing any key during the countdown, Limine freezes for around 4-5 seconds before responding to the keypress. This happens regardless of whether the key pressed is enter or an arrow key to navigate through the menu.

Resolution: The firmware present on some machines lazily initialises the keyboard stack on the very first input read. This procedure takes around 4-5 seconds, varying between computers. The issue can not be fixed in Limine, and the only known workaround might be to update the firmware to a version that does not have this issue. The problem reproduces on different bootloaders, such as systemd-boot.

Comment: Sometimes the initialisation of the keyboard stack non-deterministically fails entirely, perhaps due to Limine's use of `WaitForEvent` rather than polling. Both features should be supported by the firmware correctly.

Ticket(s): #606, #563
