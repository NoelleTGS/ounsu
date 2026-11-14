# Builds a .nro from the lx host and one island object.
#
#   make -C lx/host ISLAND=/path/island.o APP=hello OUT=/path/outdir [APP_TITLE="..."] [APP_AUTHOR="..."]
#        [APP_VERSION=x.y.z] [ICON=file.jpg (256x256 JPEG)] [LX_ROOT=...]
#
# The island comes from tools/make-island.sh. DEVKITPRO defaults to /opt/devkitpro.

LXDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)

DEVKITPRO ?= /opt/devkitpro
export DEVKITPRO
include $(DEVKITPRO)/libounce/switch2_cpu.mk

ifeq ($(strip $(ISLAND)),)
$(error ISLAND=<island object> is required)
endif

APP        ?= lx-app
APP_TITLE  ?= $(APP)
APP_AUTHOR ?= osu-kaihen
APP_VERSION ?= 0.0.0
OUT        ?= $(CURDIR)/build
ICON       ?= $(DEVKITPRO)/libounce/default_icon.jpg
# VULKAN=1: link OunceVK and hand Vulkan to the island (lx_vulkan.c).
VULKAN     ?= 0
# ROMFS=dir: bundle that folder as the .nro's romfs (read-only files the island finds when the SD card lacks them).
ROMFS      ?=
# The island's "/" on the SD card.
LX_ROOT    ?= sdmc:/switch/$(APP)

TOOLS  := $(DEVKITPRO)/tools/bin
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
CC     := $(PREFIX)gcc

ARCH    := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS  := -g -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ -D__SWITCH2__ \
           -I$(DEVKITPRO)/portlibs/switch2/include -I$(DEVKITPRO)/libounce/include -I$(LXDIR)/include \
           -DLX_ROOT='"$(LX_ROOT)"'
ASFLAGS := -g $(ARCH)
LDFLAGS := -specs=$(DEVKITPRO)/libounce/switch.specs -g $(ARCH) -Wl,--eh-frame-hdr -Wl,-Map,$(OUT)/$(APP).map \
           -L$(DEVKITPRO)/portlibs/switch2/lib -L$(DEVKITPRO)/libounce/lib
LIBS    := -lnx

SRCS := $(wildcard $(LXDIR)/host/*.c) $(wildcard $(LXDIR)/host/*.S)
ifeq ($(VULKAN),1)
CFLAGS  += -DLX_VULKAN
LIBS    := -lvulkan -lnx
else
SRCS    := $(filter-out %/lx_vulkan.c,$(SRCS))
endif
# Objects depend on the flags too (VULKAN, LX_ROOT): rebuild when they change.
FLAGS_STAMP := $(OUT)/obj/flags-$(shell echo '$(CFLAGS) $(LIBS)' | md5sum | cut -c1-12)
OBJS := $(patsubst $(LXDIR)/host/%,$(OUT)/obj/%.o,$(SRCS))

.PHONY: all clean
all: $(OUT)/$(APP).nro

$(FLAGS_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(OUT)/obj/flags-*
	@touch $@

$(OUT)/obj/%.c.o: $(LXDIR)/host/%.c $(wildcard $(LXDIR)/host/*.h) $(LXDIR)/include/lx_abi.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/%.S.o: $(LXDIR)/host/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(OUT)/$(APP).elf: $(OBJS) $(ISLAND)
	$(CC) $(LDFLAGS) $(OBJS) $(ISLAND) $(LIBS) -o $@

# Remade every time: title, author and version come from the command line.
.PHONY: $(OUT)/$(APP).nacp
$(OUT)/$(APP).nacp:
	@mkdir -p $(OUT)
	$(TOOLS)/nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

.PHONY: $(OUT)/$(APP).nro
$(OUT)/$(APP).nro: $(OUT)/$(APP).elf $(OUT)/$(APP).nacp
	$(TOOLS)/elf2nro $< $@ --icon=$(ICON) --nacp=$(OUT)/$(APP).nacp $(if $(ROMFS),--romfsdir=$(ROMFS))

clean:
	rm -rf $(OUT)
