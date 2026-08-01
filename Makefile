CC       := gcc
CFLAGS   := -Wall -O2 -std=gnu99 -D_FILE_OFFSET_BITS=64 -DINCLUDE_BIT2MCS \
              -Wno-sign-compare -Wno-unused-parameter -Wno-uninitialized \
              -Wno-format-overflow -Wno-stringop-truncation -Wno-format-truncation

# mega65fs is Linux-only (FUSE 3 + fusermount).  Refuse to build anywhere else.
UNAME_S  := $(shell uname -s 2>/dev/null || echo unknown)
ifneq ($(filter Linux%,$(UNAME_S)),Linux)
$(error mega65fs is Linux-only: requires Linux FUSE 3 and fusermount; macOS and Windows are not supported)
endif

LDFLAGS  := -pthread
CFLAGS  += $(shell pkg-config --cflags fuse3) $(shell pkg-config --cflags libusb-1.0)
LDFLAGS += $(shell pkg-config --libs fuse3) $(shell pkg-config --libs libusb-1.0)

TARGET   := mega65fs
BINDIR   := bin
BUILDDIR := build
BINARY   := $(BINDIR)/$(TARGET)

LIB_DIR  := lib/libm65ftp
INC_DIR  := $(LIB_DIR)/include
SRC_DIR  := src

# Map all object files into the build/ directory hierarchy
ETH_OBJS := \
    $(BUILDDIR)/$(LIB_DIR)/etherload/etherload_common.o \
    $(BUILDDIR)/$(LIB_DIR)/etherload/ethlet_dma_load.o \
    $(BUILDDIR)/$(LIB_DIR)/etherload/ethlet_echo.o \
    $(BUILDDIR)/$(LIB_DIR)/etherload/ethlet_all_done_basic2.o \
    $(BUILDDIR)/$(LIB_DIR)/etherload/ethlet_all_done_basic65.o \
    $(BUILDDIR)/$(LIB_DIR)/etherload/ethlet_all_done_jump.o

LIB_OBJS := \
    $(BUILDDIR)/$(LIB_DIR)/m65common.o \
    $(BUILDDIR)/$(LIB_DIR)/logging.o \
    $(BUILDDIR)/$(LIB_DIR)/ftphelper.o \
    $(BUILDDIR)/$(LIB_DIR)/ftphelper_eth.o \
    $(BUILDDIR)/$(LIB_DIR)/filehost.o \
    $(BUILDDIR)/$(LIB_DIR)/diskman.o \
    $(BUILDDIR)/$(LIB_DIR)/bit2mcs.o \
    $(BUILDDIR)/$(LIB_DIR)/version.o \
    $(ETH_OBJS)

LIB_STATIC   := $(BUILDDIR)/libm65ftp.a

# Automatically find application source files and route their objects to build/src/
APP_SRCS     := $(wildcard $(SRC_DIR)/*.c)
APP_OBJS     := $(patsubst $(SRC_DIR)/%.c,$(BUILDDIR)/$(SRC_DIR)/%.o,$(APP_SRCS))

# Keep track of all generated objects for cleaning purposes
ALL_OBJS     := $(APP_OBJS) $(LIB_OBJS)

.PHONY: all clean

all: $(BINARY)

# Link the final FUSE binary
$(BINARY): $(BINDIR) $(APP_OBJS) $(LIB_STATIC)
	$(CC) $(APP_OBJS) -o $@ $(LIB_STATIC) $(LDFLAGS)

$(BINDIR):
	mkdir -p $@

$(BUILDDIR):
	mkdir -p $@

# Build the static library from all object files
$(LIB_STATIC): $(LIB_OBJS)
	$(AR) rcs $@ $^

# Pattern rule for local application files in src/
$(BUILDDIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -I$(INC_DIR) -c $< -o $@

# Pattern rule for ordinary library .c files 
$(BUILDDIR)/$(LIB_DIR)/%.o: $(LIB_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# Pattern rule for etherload/ subdirectory 
$(BUILDDIR)/$(LIB_DIR)/etherload/%.o: $(LIB_DIR)/etherload/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# m65common.c needs libusb include path 
$(BUILDDIR)/$(LIB_DIR)/m65common.o: $(LIB_DIR)/m65common.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) $(shell pkg-config --cflags libusb-1.0) -c $< -o $@

clean:
	rm -rf $(BINARY) $(BINDIR) $(LIB_STATIC) $(BUILDDIR) $(LIB_DIR)/*.o $(LIB_DIR)/etherload/*.o $(SRC_DIR)/*.o