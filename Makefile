# OpenTyrian for the m9b SoC: builds tyrian.bin, which the core firmware's
# loader copies into PSRAM and runs (see README.md). Requires
# riscv64-unknown-elf-gcc and picolibc-riscv64-unknown-elf.

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# The m9b core (m9b-core): its firmware (SD card, FatFs) and the runtime the
# programs share (system calls, trap handler, linker script).
CORE ?= ../core
CORE_FW := $(CORE)/firmware
CORE_RT := $(CORE)/runtime

# Memory map (PSRAM, $(CORE)/runtime/m9b.ld): the image (code, rodata,
# .data) from 0x800000, then bss and the heap up to a 64 KB stack at the
# top.

B ?= build

ARCH    := -march=rv32im_zba_zicbop_zicsr_zifencei_zicbom -mabi=ilp32
CFLAGS  := $(ARCH) -O2 -g -std=gnu11 -specs=picolibc.specs --picolibc-buildtype=release \
           -ffunction-sections -fdata-sections \
           -DM9B -DTYRIAN_DIR=\"TYRIAN\" -DNDEBUG -DFF_FS_READONLY=0 \
           -Iport -Iopentyrian/src -I$(CORE_FW) -I$(CORE_FW)/fatfs \
           -Wall -Wno-unused-but-set-variable -Wno-unused-variable -Wno-missing-braces -Wno-format -Wno-format-truncation
LDFLAGS := $(ARCH) -specs=picolibc.specs --picolibc-buildtype=release \
           -Wl,--defsym=__stack_size=0x10000 \
           -Wl,--gc-sections -Wl,-Map=$(B)/tyrian.map \
           -T$(CORE_RT)/m9b.ld

# The game without its SDL video, scalers, sound output and OPL player
# (port/ replaces them), and without the network code.
GAME_SRC := $(filter-out %/video.c %/video_scale.c %/video_scale_hqNx.c %/loudness.c %/opl.c %/lds_play.c, \
            $(wildcard opentyrian/src/*.c))
PORT_SRC := $(wildcard port/*.c) $(CORE_RT)/syscalls.c $(CORE_RT)/string.c $(CORE_RT)/trap.c
PORT_ASM := $(CORE_RT)/trap_entry.S
FW_SRC   := $(CORE_FW)/sd.c $(CORE_FW)/diskio.c $(CORE_FW)/fatfs/ff.c $(CORE_FW)/fatfs/ffunicode.c
SRC      := $(GAME_SRC) $(PORT_SRC) $(FW_SRC)
OBJ      = $(patsubst %.c,$(B)/%.o,$(notdir $(SRC))) $(patsubst %.S,$(B)/%.o,$(notdir $(PORT_ASM)))

vpath %.c opentyrian/src port $(CORE_RT) $(CORE_FW) $(CORE_FW)/fatfs
vpath %.S $(CORE_RT)

CFLAGS += -MMD -MP
CFLAGS += $(EXTRA)

all: $(B)/tyrian.bin

$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/string.o: CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin

$(B)/%.o: %.S | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/tyrian.elf: $(OBJ) $(CORE_RT)/m9b.ld Makefile
	$(CC) $(LDFLAGS) $(OBJ) -lm -o $@
	$(SIZE) $@

$(B)/tyrian.bin: $(B)/tyrian.elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@

$(B):
	mkdir -p $(B)

-include $(OBJ:.o=.d)

clean:
	rm -rf build build-*

.PHONY: all clean
