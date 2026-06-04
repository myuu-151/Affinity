# Affinity PSP runtime — Mode 4 (3D) scene via sceGu/sceGum.
# Build with the pspdev toolchain:  make   (produces EBOOT.PBP)

TARGET = affinity
OBJS   = main.o source/scene.o source/meshcull.o source/mapdata.o

INCDIR   = include
CFLAGS   = -O2 -G0 -Wall -Iinclude
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBDIR  =
LDFLAGS =
# pspgum = hardware T&L matrix helpers, pspgu = GE command list, vfpu for fast math
LIBS = -lpspgum -lpspgu -lpspvfpu -lpspdisplay -lpspge -lpspctrl -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = Affinity

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
