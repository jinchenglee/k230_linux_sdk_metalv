CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy

ELF := metal-v-k230.elf
BIN := metal-v-k230.bin
OBJS := start.o main.o uart3.o

CFLAGS := -march=rv64gcv -mabi=lp64d -mcmodel=medany \
	-O2 -g -ffreestanding -fno-builtin -fno-common -fno-pic \
	-fno-stack-protector -ffunction-sections -fdata-sections \
	-msmall-data-limit=0 -Wall -Wextra
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static -Wl,--build-id=none,--gc-sections,-Map=$(ELF).map \
	-T k230.ld

.PHONY: all clean
all: $(BIN)

$(ELF): $(OBJS) k230.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

clean:
	$(RM) $(OBJS) $(ELF) $(ELF).map $(BIN)
