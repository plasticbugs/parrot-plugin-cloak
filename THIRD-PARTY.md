# What in this repository came from elsewhere

The plugin's own code, in `src/`, is under the BSD 3-Clause license
(`LICENSE`). The copy of the cart's SDK in `sdk/` is under the MIT license
(`sdk/LICENSE`). These parts came from elsewhere and keep their own terms.

| Where | What | License |
|---|---|---|
| `sdk/firmware/revc/sdk/tinyusb/` | [TinyUSB](https://github.com/hathach/tinyusb) (Ha Thach and contributors), with the cart's fixes, listed in its `VENDORED.md` | MIT (`sdk/firmware/revc/sdk/tinyusb/LICENSE`) |
| `cmake/pico_sdk_import.cmake` | from the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | BSD 3-Clause |

The built plugin also contains the Raspberry Pi Pico SDK's runtime (BSD
3-Clause) and TinyUSB (MIT).

## MAME

Parts of the code in `src/` are ports of MAME's code, or were written with
MAME's code as the reference. That code is under the BSD 3-Clause license
(`LICENSE`). It comes from MAME (<https://github.com/mamedev/mame>, tag
`mame0288`), and its copyright holders are these.

| Here | From MAME | Copyright holders |
|---|---|---|
| `src/cloak/cl.c`: the memory maps, inputs, interrupts, bitmap, graphics processor, playfield, sprites, palette | `src/mame/atari/cloak.cpp` | Dan Boris, Mirko Buffoni |
| `src/cloak/cl.c`: the POKEYs | `src/devices/sound/pokey.cpp` | Brad Oliver, Eric Smith, Juergen Buchmueller |
| `src/cloak/cl.c`: the latch | `src/devices/machine/74259.cpp` | AJR |
| `src/cloak/cl.c`: the palette's resistor weights | `src/emu/video/resnet.cpp` | Couriersud |
| `src/cloak/cl.c`: the characters and sprites decoded | `src/emu/drawgfx.cpp` | Nicola Salmoria, Aaron Giles |
| `src/cloak/cl.c`: timers, timeslices, VBLANK, the watchdog, the NVRAM | `src/emu/schedule.cpp`, `diexec.cpp`, `screen.cpp`, `sound.cpp`, `src/devices/machine/watchdog.cpp`, `nvram.cpp` | Aaron Giles |
| `src/cloak/m6502.c`, `src/cloak/m6502_body.h`, `src/cloak/tools/m6502gen.py` | `src/devices/cpu/m6502` (`m6502.cpp`, `om6502.lst`, `dm6502.lst`, `m6502make.py`) | Olivier Galibert |
| `src/cloak/cl_poly17.h`, `src/cloak/tools/poly17gen.py` | `src/devices/sound/pokey.cpp` (`poly_init_9_17`) | Brad Oliver, Eric Smith, Juergen Buchmueller |

The zip reader in `src/starwars/swzip.c` follows RFC 1951 and the method of
zlib's `contrib/puff` by Mark Adler (zlib license). It was written afresh,
for the cart's Star Wars plugin.

Cloak & Dagger is a trademark of its owner. Parrot Cart isn't connected with
them. This plugin doesn't include any game ROMs.

The 8x8 screen font's letter shapes come from the 1982 arcade game Mr. Do!
