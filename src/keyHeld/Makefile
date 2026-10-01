INCLUDE_UTILS=0
include ../common/config.mk

TARGET = keyHeld
CFLAGS := $(CFLAGS) -Os -ffunction-sections -fdata-sections -Wall
LDFLAGS := $(LDFLAGS) -Wl,--gc-sections

include ../common/commands.mk
include ../common/recipes.mk
