CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy

ELF := metal-v-k230.elf
BIN := metal-v-k230.bin
LINUX_TEST := amp-shm-test
RPMSG_TEST := rpmsg-echo-test
RPMSG_SLOT_TEST := rpmsg-slot-test
V4L2_DMA_PROBE := v4l2-dma-probe
RPMSG_ZERO_COPY_CAMERA := rpmsg-zero-copy-camera
RPMSG_LITE_DIR ?= ../../../../third_party/rpmsg-lite
RPMSG_INC := -I. -I$(RPMSG_LITE_DIR)/lib/include \
	-I$(RPMSG_LITE_DIR)/lib/include/environment/bm
RPMSG_OBJS := rpmsg_lite.o rpmsg_ns.o virtqueue.o llist.o rpmsg_env_bm.o \
	virtqueue_k230.o rpmsg_platform.o rpmsg_service.o mini_libc.o
OBJS := start.o main.o uart3.o cache.o mailbox.o $(RPMSG_OBJS)
LINUX_CC ?= riscv64-unknown-linux-gnu-gcc
LINUX_CFLAGS ?= -O2 -Wall -Wextra
HOST_CC ?= cc
HOST_SLOT_TEST := rpmsg-slot-test-host

CFLAGS := -march=rv64gcv -mabi=lp64d -mcmodel=medany $(RPMSG_INC) \
	-O2 -g -ffreestanding -fno-builtin -fno-common -fno-pic \
	-fno-stack-protector -ffunction-sections -fdata-sections \
	-msmall-data-limit=0 -Wall -Wextra
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static -Wl,--build-id=none,--gc-sections,-Map=$(ELF).map \
	-T k230.ld

.PHONY: all check clean
all: $(BIN) $(LINUX_TEST) $(RPMSG_TEST) $(RPMSG_SLOT_TEST) $(V4L2_DMA_PROBE) \
	$(RPMSG_ZERO_COPY_CAMERA)

$(ELF): $(OBJS) k230.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(LINUX_TEST): amp_shm_test.c amp_shm.h mailbox.h
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ amp_shm_test.c

$(RPMSG_TEST): rpmsg_echo_test.c rpmsg_protocol.h
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ rpmsg_echo_test.c

$(RPMSG_SLOT_TEST): rpmsg_slot_test.c rpmsg_protocol.h rpmsg_slot_state.h
	$(LINUX_CC) $(LINUX_CFLAGS) -DK230_SLOT_CAMERA -o $@ \
		rpmsg_slot_test.c -lv4l2-drm -lmmz

$(V4L2_DMA_PROBE): v4l2_dma_probe.c
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ v4l2_dma_probe.c

$(RPMSG_ZERO_COPY_CAMERA): rpmsg_zero_copy_camera.c rpmsg_protocol.h
	$(LINUX_CC) $(LINUX_CFLAGS) -o $@ rpmsg_zero_copy_camera.c

$(HOST_SLOT_TEST): rpmsg_slot_test.c rpmsg_protocol.h rpmsg_slot_state.h
	$(HOST_CC) -O2 -Wall -Wextra -Werror -o $@ rpmsg_slot_test.c

check: $(HOST_SLOT_TEST)
	./$(HOST_SLOT_TEST) --self-test

rpmsg_service.o: rpmsg_service.c rpmsg_service.h rpmsg_protocol.h \
	rpmsg_slot_state.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

rpmsg_lite.o: $(RPMSG_LITE_DIR)/lib/rpmsg_lite/rpmsg_lite.c
	$(CC) $(CFLAGS) -c -o $@ $<

rpmsg_ns.o: $(RPMSG_LITE_DIR)/lib/rpmsg_lite/rpmsg_ns.c
	$(CC) $(CFLAGS) -c -o $@ $<

virtqueue.o: $(RPMSG_LITE_DIR)/lib/virtio/virtqueue.c
	$(CC) $(CFLAGS) \
		-Dvirtqueue_get_available_buffer=virtqueue_get_available_buffer_unfixed \
		-c -o $@ $<

llist.o: $(RPMSG_LITE_DIR)/lib/common/llist.c
	$(CC) $(CFLAGS) -c -o $@ $<

rpmsg_env_bm.o: $(RPMSG_LITE_DIR)/lib/rpmsg_lite/porting/environment/rpmsg_env_bm.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) $(OBJS) $(ELF) $(ELF).map $(BIN) $(LINUX_TEST) \
		$(RPMSG_TEST) $(RPMSG_SLOT_TEST) $(V4L2_DMA_PROBE) \
		$(RPMSG_ZERO_COPY_CAMERA) $(HOST_SLOT_TEST)
