#---------------------------------------------------------------------------------
# Affinity GBA Runtime — Makefile for devkitARM
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
# path to devkitARM tools
#---------------------------------------------------------------------------------
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM

include $(DEVKITARM)/gba_rules

export LD := $(PREFIX)gcc

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
# DATA is a list of directories containing binary data
#---------------------------------------------------------------------------------
TARGET      := affinity
BUILD       := build
SOURCES     := source
INCLUDES    := include
DATA        :=

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH    := -mthumb -mthumb-interwork
CFLAGS  := -g -Wall -O2 $(ARCH) -mcpu=arm7tdmi -mtune=arm7tdmi
CFLAGS  += $(INCLUDE)
LDFLAGS  = -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS    := -ltonc

#---------------------------------------------------------------------------------
# list of directories containing libraries
#---------------------------------------------------------------------------------
LIBDIRS := $(DEVKITPRO)/libtonc

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# temporary file types
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export OFILES := $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).gba

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).gba : $(OUTPUT).elf

$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

endif
