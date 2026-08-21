EE_BIN = RepairBox.pl-PSX-HDD-Setup-v1.0.elf
EE_OBJS = src/main.o src/ui.o src/storage.o src/sha256.o src/installer.o \
	src/bootstrap.o src/bootflag_ro.o src/bootflag.o src/activation.o \
	src/direct_ready40.o src/direct_ready40_multi.o \
	src/capacity_profile.o \
	src/psx1_inspector.o src/psx1_format_test.o \
	src/psx1_pipeline.o
EE_LIBS = lib/libmc-xfrom.a -lfileXio -lpad -ldebug -lpatches -lcdvd
EE_INCS = -Iinclude -I$(PS2SDK)/ports/include
EE_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,--gc-sections

IRX_MODULES = iomanX fileXio ps2dev9 ps2atad ps2hdd ps2fs dvrdrv dvrfile usbd usbhdfsd
XFROM_MODULES = extflash xfromman xfromserv
EE_OBJS += $(addsuffix _irx.o,$(IRX_MODULES) $(XFROM_MODULES)) \
	ps2hdd_psx1_irx.o

.PHONY: all clean

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS) \
		$(addsuffix _irx.c,$(IRX_MODULES) $(XFROM_MODULES)) \
		ps2hdd_psx1_irx.c

ps2hdd_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/ps2hdd-osd.irx $@ ps2hdd_irx

ps2hdd_psx1_irx.c: irx/psx1/ps2hdd-sparse-skip.irx
	$(PS2SDK)/bin/bin2c $< $@ ps2hdd_psx1_irx

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $*_irx

$(addsuffix _irx.c,$(XFROM_MODULES)): %_irx.c: irx/%.irx
	$(PS2SDK)/bin/bin2c $< $@ $*_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
