CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -std=gnu99 -D_FILE_OFFSET_BITS=64 -DINCLUDE_BIT2MCS
LDFLAGS  := -pthread
CFLAGS  += $(shell pkg-config --cflags fuse3) $(shell pkg-config --cflags libusb-1.0)
LDFLAGS += $(shell pkg-config --libs fuse3) $(shell pkg-config --libs libusb-1.0)

TARGET   := mega65fs
BINDIR   := bin
BINARY   := $(BINDIR)/$(TARGET)

LIB_DIR  := libm65ftp
INC_DIR  := $(LIB_DIR)/include
SRC_DIR  := src

ETH_OBJS := \
	$(LIB_DIR)/etherload/etherload_common.o \
	$(LIB_DIR)/etherload/ethlet_dma_load.o \
	$(LIB_DIR)/etherload/ethlet_echo.o \
	$(LIB_DIR)/etherload/ethlet_all_done_basic2.o \
	$(LIB_DIR)/etherload/ethlet_all_done_basic65.o \
	$(LIB_DIR)/etherload/ethlet_all_done_jump.o

LIB_OBJS := \
	$(LIB_DIR)/m65ftp_lib.o \
	$(LIB_DIR)/m65common.o \
	$(LIB_DIR)/logging.o \
	$(LIB_DIR)/ftphelper.o \
	$(LIB_DIR)/ftphelper_eth.o \
	$(LIB_DIR)/filehost.o \
	$(LIB_DIR)/diskman.o \
	$(LIB_DIR)/bit2mcs.o \
	$(LIB_DIR)/version.o \
	$(ETH_OBJS)

LIB_ALL_OBJS := $(LIB_OBJS) $(LIB_DIR)/etherload_common.o $(LIB_DIR)/dump_hex.o $(LIB_DIR)/ethlet_*.o
LIB_STATIC   := $(LIB_DIR)/libm65ftp.a

.PHONY: all clean

all: $(BINARY)

# Link the final FUSE binary into bin/
$(BINARY): $(BINDIR) $(SRC_DIR)/mega65fs.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $(SRC_DIR)/mega65fs.c -o $@ -I$(SRC_DIR) -I$(INC_DIR) $(LIB_STATIC) $(LDFLAGS)

$(BINDIR):
	mkdir -p $@

# Build the static library from all object files
$(LIB_STATIC): $(LIB_OBJS)
	$(AR) rcs $@ $^

# Pattern rule for ordinary library .c files
$(LIB_DIR)/%.o: $(LIB_DIR)/%.c
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# Pattern rule for etherload/ subdirectory
$(LIB_DIR)/etherload/%.o: $(LIB_DIR)/etherload/%.c
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# m65ftp_lib.o includes mega65_ftp.c textually
$(LIB_DIR)/m65ftp_lib.o: $(LIB_DIR)/m65ftp_lib.c $(LIB_DIR)/mega65_ftp.c
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

# m65common.c needs libusb include path
$(LIB_DIR)/m65common.o: $(LIB_DIR)/m65common.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $(shell pkg-config --cflags libusb-1.0) -c $< -o $@

clean:
	rm -rf $(BINARY) $(BINDIR) $(LIB_STATIC) $(LIB_OBJS) $(LIB_ALL_OBJS)
