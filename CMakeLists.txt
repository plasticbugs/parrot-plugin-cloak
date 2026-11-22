# cloak (CLOAK.CVP): Atari's 1983 Cloak & Dagger arcade game on the
# ColecoVision. The cart runs the arcade board (two 6502s, the bitmap and
# two POKEYs: src/cloak/cl.c, from MAME 0.288) from the user's ROMs on the card
# (/CLOAK), and clcv sends the picture and the sound to the console as the
# Game Boy kernel's lists. RESET goes back to the menu.
#
# The ROMs (120K), the bitmaps (64K) and the code (250K) are more than
# SRAM holds, so this is a flash binary run in place (XIP) from its slot,
# with the code a list runs in SRAM: CL_HOT, M65_HOT and CLCV_HOT put it in
# .time_critical, which crt0 copies there, and M65_DATA, CL_DATA and
# CLCV_DATA the few small tables it reads. The characters and the sprites
# (16K) are read into the bitmaps' room, which cl_init clears once it has
# decoded them. The POKEY's 17-bit polynomial (16K, cl_poly17.h) stays in
# flash: about 280 of its 8-byte lines a frame go through the XIP cache.
#
#   cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
#   ninja -C build                      -> build/CLOAK.CVP
#
# Copy CLOAK.CVP into the card's /PLUGINS folder.
cmake_minimum_required(VERSION 3.13)

# the cart's SDK: the copy in sdk/ (sdk/README.md)
set(PARROTCART_SDK ${CMAKE_CURRENT_LIST_DIR}/sdk/firmware/revc)
set(PICO_BOARD_HEADER_DIRS ${PARROTCART_SDK}/cvcart_revc/boards)
set(PICO_BOARD cvcart_revc)
set(PICO_PLATFORM rp2350-arm-s)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# the USB host: sdk/tinyusb (TinyUSB 0.21 and its fixes since), not the pico-sdk's 0.18
include(${PARROTCART_SDK}/sdk/cvpad_tinyusb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(cloak C CXX ASM)
pico_sdk_init()
include(${PARROTCART_SDK}/sdk/cvplugin.cmake)

# The same source makes the same file, whenever it's built: the compiler's
# __DATE__ and __TIME__ (the build date in the binary info) come from
# SOURCE_DATE_EPOCH, fixed here at the tested build's day, 2026-11-21.
set(BUILD_EPOCH 1795262400)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})

# the plugin's code (the cart's firmware/revc/cloak/, with plugin/ beside it)
set(S ${CMAKE_CURRENT_LIST_DIR}/src/cloak)

set(OURS ${S}/cloak.c ${S}/cl.c ${S}/m6502.c ${S}/clcv.c ${S}/clrun.c ${S}/clcard.c ${S}/clroms.c ${CMAKE_CURRENT_LIST_DIR}/src/starwars/swzip.c)
set_source_files_properties(${OURS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wno-unused-function;-fstack-usage")
# the emulation's state is many small statics: in one section each, every
# use needs a separate address loaded, so not
set_source_files_properties(${S}/cl.c PROPERTIES COMPILE_OPTIONS
                            "-Wall;-Wno-unused-function;-fstack-usage;-fno-data-sections")
# the 6502's one computed-goto body: GCC's code motion and its block
# reordering make each op's path longer (test/armcount.sh measures it), and
# no tracing
set_source_files_properties(${S}/m6502.c PROPERTIES COMPILE_OPTIONS
                            "-Wall;-Wno-unused-function;-fstack-usage;-fno-data-sections;-fno-gcse;-fno-tree-pre;-fno-code-hoisting;-freorder-blocks-algorithm=simple;-DM65_NO_TRACE")
# the card's code runs once, at the start: built for size
set_source_files_properties(${S}/clcard.c ${S}/clroms.c ${CMAKE_CURRENT_LIST_DIR}/src/starwars/swzip.c PROPERTIES COMPILE_OPTIONS
                            "-Wall;-Wno-unused-function;-fstack-usage;-Os")
set_source_files_properties(${PARROTCART_SDK}/sdplayer/fsro.c PROPERTIES COMPILE_OPTIONS "-Os")

add_executable(cloak ${OURS} ${PARROTCART_SDK}/midihello/vdp.c)
target_include_directories(cloak PRIVATE ${S} ${PARROTCART_SDK}/midihello
                           ${PARROTCART_SDK}/sdk)
# the sound's bins: a list takes 100 (and the lead's few)
# core 0's stack: all of scratch Y (4K; a list goes 2.2K deep, test/armcount.sh);
# core 1's as before (scratch X also holds its bus loop)
target_compile_definitions(cloak PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0 PICO_PRINTF_SUPPORT_EXPONENTIAL=0 CL_NBIN=128
    PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x800)
cvframes_use(cloak)
cvframes_serve2(cloak)      # core 1's bus loop acts on two samples of the pins that agree (sdk/cvfserve.S)
cvsd_use(cloak)
cvpad_use(cloak)                            # USB game pads: the port is a host
pico_enable_stdio_usb(cloak 0)
pico_enable_stdio_uart(cloak 0)
cvcart_plugin(cloak ID parrotcart.cloak NAME "Cloak & Dagger" VERSION 0.1.2 BY "Parrot Cart"
              HOME /CLOAK USES usb-host ICON src/cloak/cloak_icon.txt FILE CLOAK.CVP README CART-README.md XIP)
