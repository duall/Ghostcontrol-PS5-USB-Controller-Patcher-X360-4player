PS5_HOST ?=
PORT     ?= 9021
TARGET   := ghost-control-easysmx-x10.elf
RD_TARGET := ghost-control-8bitdo-arcade-stick-rd.elf
ARCADE_SWITCH_TARGET := ghost-control-8bitdo-arcade-stick-switch.elf
BUTTON_MAP_TARGET := ghost-control-8bitdo-arcade-stick-button-map.elf
XINPUT_BUTTON_MAP_TARGET := ghost-control-8bitdo-arcade-stick-xinput-button-map.elf

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS  += -D__PROSPERO__ -Wall -Wextra -g -O2 -fPIC -fno-stack-protector
LDFLAGS += -lScePad -lSceUserService -lpthread -ldl

SRC := gc_main.c shellui_pad.c usb_helpers.c controller_nintendo.c controller_xbox.c controller_ds4.c controller_mamba.c

.PHONY: all rd arcade-switch button-map xinput-button-map clean deploy

all: $(TARGET)

rd: $(RD_TARGET)

arcade-switch: $(ARCADE_SWITCH_TARGET)

button-map: $(BUTTON_MAP_TARGET)

xinput-button-map: $(XINPUT_BUTTON_MAP_TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(RD_TARGET): arcade_stick_rd.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ARCADE_SWITCH_TARGET): $(SRC)
	$(CC) $(CFLAGS) '-DGC_BUILD_LABEL="8BitDo Arcade Stick Switch mode"' '-DGC_SWITCH_CONTROLLER_NAME="8BitDo Arcade Stick (Switch dongle)"' -o $@ $^ $(LDFLAGS)

$(BUTTON_MAP_TARGET): arcade_stick_rd.c
	$(CC) $(CFLAGS) -DGC_BUTTON_MAP_PROBE=1 -o $@ $^ $(LDFLAGS)

$(XINPUT_BUTTON_MAP_TARGET): arcade_stick_rd.c
	$(CC) $(CFLAGS) -DGC_BUTTON_MAP_PROBE=1 -DGC_XINPUT_MAP_PROBE=1 -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET) $(RD_TARGET) $(ARCADE_SWITCH_TARGET) $(BUTTON_MAP_TARGET) $(XINPUT_BUTTON_MAP_TARGET)

deploy: $(TARGET)
ifndef PS5_HOST
	$(error PS5_HOST is not set)
endif
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)
