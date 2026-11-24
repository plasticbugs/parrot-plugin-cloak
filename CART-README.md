# Cloak & Dagger

This plugin plays Atari's 1983 Cloak & Dagger arcade game on the ColecoVision. It needs the game's ROMs from MAME, which you supply.

## Setup

1. Copy `CLOAK.CVP` into the `/PLUGINS` folder on the card.
2. Make a folder named `/CLOAK` and put MAME's `cloak.zip` in it as it is, or the files from it loose. You can also put the zip in the card's top folder as `/CLOAK.ZIP`, but then the high scores aren't saved. Saving them needs the `/CLOAK` folder.
3. Start Cloak & Dagger from the Plugins page in the menu.

The files can have any names, because the plugin knows each ROM by its size and contents. If a ROM is missing, the screen names it.

## Controls

The arcade game has two 8-way sticks, one to move Agent X and one to fire, and a button.

On the ColecoVision controller:

- The joystick moves Agent X.
- Hold the left fire button and the joystick fires that way instead.
- The right fire button is the arcade's button.
- Keypad * or # adds a coin.
- Keypad 1 starts a one-player game, and keypad 2 a two-player game.
- Press RESET on the console to go back to the menu, or hold both fire buttons and \* for 2 seconds (Select and Start on a USB pad).

On a USB pad, through a powered hub:

- The D-pad or the left stick moves Agent X.
- The right stick fires that way. The four face buttons fire too, each its own way (left, up, right, down), for pads with one stick.
- A shoulder button is the arcade's button.
- Minus adds a coin, and Plus starts a one-player game.

## Picture and sound

The arcade screen is 256 by 232 pixels. The ColecoVision shows 256 by 192, so the plugin joins two lines into one in each band of eight, or two pairs in some bands, and keeps any line one pixel thin. Text and the floors' edges stay whole.

Colors are the nearest of the ColecoVision's 15. Each run of eight pixels can only have two of them, so a few edges where three colors meet look rougher than in the arcade.

The two sound chips play through the ColecoVision's sound chip as 5,962 samples a second.

The high scores are saved in `/CLOAK/CLOAK.NV` when you press RESET to go back to the menu. The file is the same as MAME's `nvram/cloak/nvram`, so you can copy it either way.

## Credits

The arcade emulation is a port of MAME 0.288's code, under the BSD-3-Clause license. Its authors are:

- Dan Boris and Mirko Buffoni, for the Cloak & Dagger driver.
- Olivier Galibert, for the 6502 CPU.
- Brad Oliver, Eric Smith and Juergen Buchmueller, for the POKEY sound chip.
- Aaron Giles, for MAME's scheduler, screen and NVRAM.
- Nicola Salmoria and Aaron Giles, for the graphics decoding.
- AJR, for the 74LS259 latch.
- Couriersud, for the resistor network colors.

You can find MAME at https://www.mamedev.org. The zip reader follows Mark Adler's puff, from zlib. USB support comes from TinyUSB by Ha Thach, under the MIT license.

The plugin itself is by Parrot Cart, and it's free. Its source is linked next to its download.

Cloak & Dagger is a trademark of its owner. This plugin contains none of the game.
