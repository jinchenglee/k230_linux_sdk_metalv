CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy

ELF := metal-v-k230.elf
BIN := metal-v-k230.bin
LINUX_TEST := amp-shm-test
RPMSG_TEST := rpmsg-echo-test
RPMSG_LITE_DIR ?= ../../../../third_party/rpmsg-lite
RPMSG_INC := -I. -I$(RPMSG_LITE_DIR)/lib/include \
	-I$(RPMSG_LITE_DIR)/lib/include/environment/bm
RPMSG_OBJS := rpmsg_lite.o rpmsg_ns.o virtqueue.o llist.o rpmsg_env_bm.o \
	rpmsg_platform.o rpmsg_service.o mini_libc.o
OBJS := start.o main.o uart3.o cache.o mailbox.o $(RPMSG_OBJS)
LINUX_CC ?= riscv64-unknown-linux-gnu-gcc
LINUX_CFLAGS ?= -O2 -Wall -Wextra

CFLAGS := -march=rv64gcv -mabi=lp64d -mcmodel=medany $(RPMSG_INC) \
	-O2 -g -ffreestanding -fno-builtin -fno-common -fno-pic \
	-fno-stack-protector -ffunction-sections -fdata-sections \
	-msmall-data-limit=0 -Wall -Wextra
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static -Wl,--build-id=none,--gc-sections,-Map=$(ELF).map \
	-T k230.ld

.PHONY: all clean
all: $(BIN) $(LINUX_TEST) $(RPMSG_TEST)

$(ELF): $(OBJS) k230.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(LINUX_TEST): amp_shm_test.c amp_shm.h mailbox.h
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ amp_shm_test.c

$(RPMSG_TEST): rpmsg_echo_test.c
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ rpmsg_echo_test.c

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

rpmsg_lite.o: $(RPMSG_LITE_DIR)/lib/rpmsg_lite/rpmsg_lite.c
	$(CC) $(CFLAGS) -c -o $@ $<

rpmsg_ns.o: $(RPMSG_LITE_DIR)/lib/rpmsg_lite/rpmsg_ns.c
	$(CC) $(CFLAGS) -c -o $@ $<

virtqueue.o: $(RPMSG_LITE_DIR)/lib/virtio/virtqueue.c
	$(CC) $(CFLAGS) -c -o $@ $<

llist.o: $(RPMSG_LITE_DIR)/lib/common/llist.c
	$(CC) $(CFLAGS) -c -o $@ $<

rpmsg_env_bm.o: $(RPMSG_LITE_DIR)/lib/rpmsg_lite/porting/environment/rpmsg_env_bm.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) $(OBJS) $(ELF) $(ELF).map $(BIN) $(LINUX_TEST) \
		$(RPMSG_TEST)
