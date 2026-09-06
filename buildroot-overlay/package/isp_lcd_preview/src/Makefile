#
# Makefile — isp_lcd_preview (universal camera preview)
#

BIN = isp_lcd_preview

INCS += -I$(STAGING_DIR)/usr/include -I$(STAGING_DIR)/usr/include/libdrm
LIBPATH += -L$(STAGING_DIR)/usr/lib

# Link only the libs the API actually needs (matches v4l2-drm.pc: -lv4l2-drm
# + -ldisplay). Do NOT link -lvvcam directly: this SDK's apps never do.
LIBS += -lv4l2-drm -ldisplay -lpthread -lm

CFLAGS ?=
LDFLAGS ?=

SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)

all: $(BIN)

$(OBJS): %.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@ $(INCS)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $(BIN) $(OBJS) $(LIBPATH) $(LIBS)
	@rm -f $(OBJS) $(BIN).map

clean:
	rm -f $(BIN) $(OBJS)
