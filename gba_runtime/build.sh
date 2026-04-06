#!/bin/bash
# Affinity GBA build script
set -e

export PATH="/c/devkitPro/devkitARM/bin:/c/devkitPro/tools/bin:$PATH"

SRC=../source
INC="-I/c/devkitPro/libtonc/include -I../include"
CFLAGS="-g -O2 -mcpu=arm7tdmi -mtune=arm7tdmi -mthumb -mthumb-interwork $INC"
AFLAGS="-marm -O2 -mthumb-interwork -mcpu=arm7tdmi"
LDFLAGS="-g -mthumb -mthumb-interwork -Wl,-Map,affinity.map -specs=gba.specs"
LIBS="-L/c/devkitPro/libtonc/lib -ltonc"

cd "$(dirname "$0")/build"

echo "main.c"
arm-none-eabi-gcc $CFLAGS -c $SRC/main.c -o main.o

echo "tex_iwram.c"
arm-none-eabi-gcc $CFLAGS -c $SRC/tex_iwram.c -o tex_iwram.o

echo "tex_scanline.s"
arm-none-eabi-gcc $AFLAGS -c $SRC/tex_scanline.s -o tex_scanline.o

echo "hline_fast.s"
arm-none-eabi-gcc $AFLAGS -c $SRC/hline_fast.s -o hline_fast.o

echo "rasterize_convex_asm.s"
arm-none-eabi-gcc $AFLAGS -c $SRC/rasterize_convex_asm.s -o rasterize_convex_asm.o

echo "rasterize_convex_tex_asm.s"
arm-none-eabi-gcc $AFLAGS -c $SRC/rasterize_convex_tex_asm.s -o rasterize_convex_tex_asm.o

echo "linking cartridge"
arm-none-eabi-gcc $LDFLAGS main.o tex_scanline.o hline_fast.o rasterize_convex_asm.o rasterize_convex_tex_asm.o tex_iwram.o $LIBS -o affinity.elf
arm-none-eabi-objcopy -O binary affinity.elf affinity.gba
gbafix affinity.gba
cp affinity.gba ../

echo "Done: affinity.gba"
