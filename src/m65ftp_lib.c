/*
 * m65ftp_lib.c — local wrapper API over the vendored MEGA65-FTP library.
 *
 * The vendored upstream tool (lib/libm65ftp/mega65_ftp.c) is compiled
 * inline into this translation unit behind the M65FTP_AS_LIB guard.  Every
 * call here serialises on s_mutex, so FUSE-style multi-threaded callers are
 * safe.
 *
 * Writing model: m65ftp_create() allocates no SD clusters up front.  Data is
 * accumulated in a host-memory write buffer and flushed to the SD card as a
 * single contiguous cluster block by flush_write_buffer() on close (or when
 * the buffer would exceed M65FTP_BUFFER_LIMIT).  This keeps every file
 * created through this API unfragmented.
 */

#define M65FTP_AS_LIB

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "../lib/libm65ftp/mega65_ftp.c"

#include "m65ftp_lib.h"

/* ---------------------------------------------------------------------
 * Internal constants
 * ------------------------------------------------------------------- */

/* FAT32 directory-entry field offsets (within the 32-byte entry). */
#define DIR_ENTRY_ATTR 0x0B
#define DIR_ENTRY_ACCESS_DATE 0x12
#define DIR_ENTRY_CLUSTER_HI 0x14
#define DIR_ENTRY_WRITE_TIME 0x16
#define DIR_ENTRY_WRITE_DATE 0x18
#define DIR_ENTRY_CLUSTER_LO 0x1A
#define DIR_ENTRY_FILE_SIZE 0x1C

/* FAT attribute bits. */
#define FAT_ATTR_DIRECTORY 0x10

/* Master boot record layout. */
#define MBR_SIGNATURE_LO 510
#define MBR_SIGNATURE_HI 511
#define MBR_PARTITION_TABLE 0x1BE
#define MBR_PARTITION_ENTRY_SIZE 16
#define MBR_PARTITION_TYPE 4
#define MBR_PARTITION_START_LBA 8
#define MBR_PARTITION_SECTORS 12

/* Wrapper limits. */
#define M65FTP_MAX_FILES 4096
#define M65FTP_BUFFER_LIMIT (64UL * 1024 * 1024)
#define M65FTP_PATH_MAX 1024
#define M65FTP_NAME_MAX 256
#define M65FTP_DIR_BUF_SIZE 512

/* ---------------------------------------------------------------------
 * Per-descriptor state and file-scope data
 * ------------------------------------------------------------------- */

struct m65ftp_fileinfo {
    unsigned int first_cluster;
    unsigned int file_size;
    unsigned int current_cluster;     /* last resolved cluster for sequential continuation */
    unsigned int current_cluster_idx; /* logical cluster index of current_cluster */
    off_t last_offset;                /* byte offset where the last pread ended */
    /* Write state */
    int is_writable;
    int dir_sector;                   /* sector of the directory entry */
    int dir_sector_offset;            /* offset of the entry within dir_sector */
    unsigned char dir_sector_buf[M65FTP_DIR_BUF_SIZE]; /* cached dir sector for finalising */
    int buffered;                     /* non-zero = use host-memory writeback buffering */
    unsigned char *write_buf;         /* host-memory write buffer (NULL = not allocated) */
    size_t write_buf_cap;             /* allocated capacity of write_buf */
    unsigned int write_cluster;       /* current cluster being written to */
    int sector_in_cluster;            /* sector index within write_cluster */
    int in_use;                       /* 0 once closed and returned to the free-fd pool */
};

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_initialized = 0;
static int s_file_count = 0;
static struct m65ftp_fileinfo s_files[M65FTP_MAX_FILES];

/* Stack of fd slots released by m65ftp_close(), reused before growing
   s_file_count.  Without this, every open()/create() over the life of a
   mount permanently consumes a slot and the pool eventually runs out. */
static int s_free_fds[M65FTP_MAX_FILES];
static int s_free_count = 0;

/* Must be called with s_mutex held.  Returns a fresh or recycled fd, or -1
   if the pool of M65FTP_MAX_FILES concurrently-open descriptors is full. */
static int alloc_fd(void)
{
    if (s_free_count > 0)
        return s_free_fds[--s_free_count];
    if (s_file_count >= M65FTP_MAX_FILES)
        return -1;
    return s_file_count++;
}

/* ---------------------------------------------------------------------
 * Static helpers
 * ------------------------------------------------------------------- */

/* Pack a 16/32-bit little-endian value into a directory-entry buffer. */
static void pack_dir_entry_le16(unsigned char *buffer, int offset,
                                unsigned int value)
{
    buffer[offset] = value & 0xFF;
    buffer[offset + 1] = (value >> 8) & 0xFF;
}

static void pack_dir_entry_le32(unsigned char *buffer, int offset,
                                unsigned int value)
{
    buffer[offset] = value & 0xFF;
    buffer[offset + 1] = (value >> 8) & 0xFF;
    buffer[offset + 2] = (value >> 16) & 0xFF;
    buffer[offset + 3] = (value >> 24) & 0xFF;
}

/* Update the cached directory entry of a buffered file. */
static void set_entry_first_cluster(struct m65ftp_fileinfo *fi,
                                    unsigned int first_cluster)
{
    int offset = fi->dir_sector_offset;
    pack_dir_entry_le16(fi->dir_sector_buf, offset + DIR_ENTRY_CLUSTER_LO,
                        first_cluster & 0xFFFF);
    pack_dir_entry_le16(fi->dir_sector_buf, offset + DIR_ENTRY_CLUSTER_HI,
                        (first_cluster >> 16) & 0xFFFF);
}

static void set_entry_file_size(struct m65ftp_fileinfo *fi,
                                unsigned int file_size)
{
    pack_dir_entry_le32(fi->dir_sector_buf,
                        fi->dir_sector_offset + DIR_ENTRY_FILE_SIZE,
                        file_size);
}

/*
 * Flush a buffered file's write_buf to a contiguous SD cluster chain.
 * Must be called with s_mutex held.  On success the buffer is freed and the
 * descriptor switches to direct (non-buffered) mode with first_cluster set.
 */
static int flush_write_buffer(struct m65ftp_fileinfo *fi)
{
    if (fi->file_size == 0) {
        if (fi->dir_sector >= 0 && fi->dir_sector_offset >= 0) {
            set_entry_first_cluster(fi, 0);
            set_entry_file_size(fi, 0);
            write_sector(partition_start + fi->dir_sector, fi->dir_sector_buf);
        }
        free(fi->write_buf);
        fi->write_buf = NULL;
        fi->write_buf_cap = 0;
        fi->buffered = 0;
        fi->first_cluster = 0;
        return 0;
    }

    unsigned int cluster_size = sectors_per_cluster * 512;
    unsigned int needed = (fi->file_size + cluster_size - 1) / cluster_size;

    unsigned int start = find_contiguous_clusters(needed);
    if (!start || start >= FAT32_MIN_END_OF_CLUSTER_MARKER)
        return -1;

    /* Allocate and chain the contiguous block. */
    allocate_cluster(start);
    for (unsigned int i = 1; i < needed; i++) {
        unsigned int cluster = start + i;
        allocate_cluster(cluster);
        chain_cluster(start + i - 1, cluster);
    }

    /* Write data sectors (zero-fill gaps beyond the buffer, e.g. sparse
       ftruncate). */
    unsigned int first_data_sector = partition_start + reserved_sectors +
        2 * sectors_per_fat;
    unsigned char sector_buffer[M65FTP_DIR_BUF_SIZE];
    size_t remaining = fi->file_size;
    unsigned int cluster = start;
    while (remaining > 0) {
        for (unsigned int sec = 0; sec < sectors_per_cluster && remaining > 0;
             sec++) {
            unsigned int sector_number = first_data_sector +
                (cluster - 2) * sectors_per_cluster + sec;
            size_t off = (size_t)(cluster - start) * cluster_size + sec * 512;
            size_t copy = 512;
            if (copy > remaining) copy = remaining;
            memset(sector_buffer, 0, M65FTP_DIR_BUF_SIZE);
            if (fi->write_buf && off < fi->write_buf_cap) {
                size_t buf_copy = copy;
                if (off + buf_copy > fi->write_buf_cap)
                    buf_copy = fi->write_buf_cap - off;
                memcpy(sector_buffer, fi->write_buf + off, buf_copy);
            }
            if (write_sector(sector_number, sector_buffer))
                return -1;
            remaining -= copy;
        }
        cluster++;
    }

    /* Persist the new first_cluster and file_size in the directory entry. */
    if (fi->dir_sector >= 0 && fi->dir_sector_offset >= 0) {
        set_entry_first_cluster(fi, start);
        set_entry_file_size(fi, fi->file_size);
        write_sector(partition_start + fi->dir_sector, fi->dir_sector_buf);
    }

    fi->first_cluster = start;
    fi->write_cluster = start;
    fi->sector_in_cluster = 0;
    fi->last_offset = 0;
    fi->current_cluster = start;
    fi->current_cluster_idx = 0;

    free(fi->write_buf);
    fi->write_buf = NULL;
    fi->write_buf_cap = 0;
    fi->buffered = 0;
    return 0;
}

/* Count the clusters in a chain starting at first; store the last cluster. */
static int count_cluster_chain(unsigned int first_cluster,
                               unsigned int *last_cluster)
{
    unsigned int count = 1;
    unsigned int cluster = first_cluster;
    while (1) {
        unsigned int next = chained_cluster(cluster);
        if (next == 0 || next >= FAT32_MIN_END_OF_CLUSTER_MARKER)
            break;
        cluster = next;
        count++;
    }
    *last_cluster = cluster;
    return count;
}

/* ---------------------------------------------------------------------
 * Partition information
 * ------------------------------------------------------------------- */

int m65ftp_mbrinfo(struct m65ftp_partition partitions[4])
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    unsigned char mbr[M65FTP_DIR_BUF_SIZE];
    /* The MBR always lives at absolute sector 0 on the SD card. */
    if (read_sector(0, mbr, CACHE_NO, 0) != 0) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    /* Check the MBR signature 0x55AA at offsets 510-511. */
    if (mbr[MBR_SIGNATURE_LO] != 0x55 || mbr[MBR_SIGNATURE_HI] != 0xAA) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    int count = 0;
    /* The partition table starts at offset 0x1BE (4 entries of 16 bytes). */
    for (int i = 0; i < 4; i++) {
        int base = MBR_PARTITION_TABLE + (i * MBR_PARTITION_ENTRY_SIZE);
        unsigned char type = mbr[base + MBR_PARTITION_TYPE];

        /* Little-endian 32-bit integers for start LBA and sector count. */
        unsigned int start_lba =
            mbr[base + MBR_PARTITION_START_LBA] |
            (mbr[base + MBR_PARTITION_START_LBA + 1] << 8) |
            (mbr[base + MBR_PARTITION_START_LBA + 2] << 16) |
            (mbr[base + MBR_PARTITION_START_LBA + 3] << 24);

        unsigned int sectors =
            mbr[base + MBR_PARTITION_SECTORS] |
            (mbr[base + MBR_PARTITION_SECTORS + 1] << 8) |
            (mbr[base + MBR_PARTITION_SECTORS + 2] << 16) |
            (mbr[base + MBR_PARTITION_SECTORS + 3] << 24);

        partitions[i].type = type;
        partitions[i].start_sector = start_lba;
        partitions[i].sector_count = sectors;

        if (type != 0 && sectors > 0)
            count++;
    }

    pthread_mutex_unlock(&s_mutex);
    return count;
}

/* ---------------------------------------------------------------------
 * Connection lifecycle
 * ------------------------------------------------------------------- */

int m65ftp_init(void)
{
    pthread_mutex_lock(&s_mutex);
    if (s_initialized) {
        pthread_mutex_unlock(&s_mutex);
        return 0;
    }

    log_setup(stderr, LOG_WARN);
    direct_sdcard_device = 0;
    ethernet_mode = 1;
    file_system_found = 0;
    first_cluster = 0;
    partition_start = 0xffffffff;
    sdhc = -1;
    current_dir[0] = '/';
    current_dir[1] = 0;

    if (etherload_init(NULL, NULL)) {
        log_error("Unable to initialize ethernet communication");
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    ethl_setup_dmaload();

    if (trigger_eth_hyperrupt() < 0) {
        etherload_finish();
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    if (ethl_ping(3000) < 0) {
        log_error("No response from MEGA65");
        log_error("Please make sure ETHLOAD.M65 is available in the root folder of the SD card.");
        etherload_finish();
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    log_info("Starting helper routine transfer...");
    unsigned char *helper_ptr = helperroutine_eth + 2;
    int bytes = helperroutine_eth_len - 2;
    int address = 0x0801;
    int block_size = 1024;

    while (bytes > 0) {
        if (bytes < block_size)
            block_size = bytes;
        if (send_mem(address, helper_ptr, block_size, ETHERNET_TIMEOUT)) {
            log_error("No response from MEGA65");
            etherload_finish();
            pthread_mutex_unlock(&s_mutex);
            return -1;
        }
        helper_ptr += block_size;
        address += block_size;
        bytes -= block_size;
    }
    wait_all_acks(3000);
    log_info("Helper routine transfer complete");

    ethlet_all_done_basic2[ethlet_all_done_basic2_offset_data_end_address] = 0x01;
    ethlet_all_done_basic2[ethlet_all_done_basic2_offset_data_end_address + 1] = 0x08;
    ethlet_all_done_basic2[ethlet_all_done_basic2_offset_do_run] = 1;
    ethlet_all_done_basic2[ethlet_all_done_basic2_offset_enable_cart_signature] = 0;

    if (ethl_single_command((uint8_t *)ethlet_all_done_basic2,
                            ethlet_all_done_basic2_len, 2000) < 0) {
        log_error("No response from MEGA65");
        etherload_finish();
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    sockfd = ethl_get_socket();
    ethl_setup_callbacks(&ethernet_get_packet_seq, &ethernet_match_payloads,
        &ethernet_is_duplicate, &ethernet_embed_packet_seq,
        ethernet_timeout_handler);

    ethernet_login();
    log_info("Login successful");

    determine_ethernet_window_size();
    sdhc_check();

    if (!file_system_found)
        open_file_system();

    s_initialized = 1;
    pthread_mutex_unlock(&s_mutex);
    return 0;
}

void m65ftp_finish(void)
{
    pthread_mutex_lock(&s_mutex);
    if (s_initialized) {
        request_quit();
        etherload_finish();
        s_initialized = 0;
    }
    pthread_mutex_unlock(&s_mutex);
}

/* ---------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------- */

int m65ftp_open(const char *path)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }

    char dir_path[M65FTP_PATH_MAX];
    char file_name[M65FTP_NAME_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&s_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (fat_opendir(dir_path, 0)) { pthread_mutex_unlock(&s_mutex); return -1; }

    struct m65dirent dir_entry;
    while (fat_readdir(&dir_entry, 0) == 0) {
        if (dir_entry.d_name[0] == 0)
            continue;
        char *name = dir_entry.d_longname[0] ? dir_entry.d_longname : dir_entry.d_name;
        if (!strcmp(name, file_name)) {
            int fd = alloc_fd();
            if (fd < 0) { pthread_mutex_unlock(&s_mutex); return -1; }
            s_files[fd].first_cluster = dir_entry.d_ino;
            s_files[fd].file_size = dir_entry.d_filelen;
            s_files[fd].current_cluster = dir_entry.d_ino;
            s_files[fd].current_cluster_idx = 0;
            s_files[fd].last_offset = 0;
            s_files[fd].is_writable = 0;
            s_files[fd].buffered = 0;
            s_files[fd].write_buf = NULL;
            s_files[fd].write_buf_cap = 0;
            memset(s_files[fd].dir_sector_buf, 0, M65FTP_DIR_BUF_SIZE);
            s_files[fd].dir_sector = -1;
            s_files[fd].write_cluster = 0;
            s_files[fd].sector_in_cluster = 0;
            s_files[fd].in_use = 1;
            pthread_mutex_unlock(&s_mutex);
            return fd;
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return -1;
}

int m65ftp_create(const char *path)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }

    char dir_path[M65FTP_PATH_MAX];
    char file_name[M65FTP_NAME_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&s_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!file_name[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    char saved_dir[M65FTP_PATH_MAX];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    if (fat_opendir(dir_path, 1)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    struct m65dirent dir_entry;
    if (find_file_in_curdir(file_name, &dir_entry)) {
        delete_file_or_dir(file_name);
        fat_opendir(dir_path, 1);
    }

    if (!create_direntry_with_attrib(file_name, DE_ATTRIB_FILE)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }
    /* Persist the directory entry with first_cluster = 0 (clusters are
       allocated on close). */
    write_sector(partition_start + dir_sector, dir_sector_buffer);

    int fd = alloc_fd();
    if (fd < 0) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }
    s_files[fd].first_cluster = 0;
    s_files[fd].file_size = 0;
    s_files[fd].current_cluster = 0;
    s_files[fd].current_cluster_idx = 0;
    s_files[fd].last_offset = 0;
    s_files[fd].is_writable = 1;
    s_files[fd].buffered = 1;
    s_files[fd].write_buf = NULL;
    s_files[fd].write_buf_cap = 0;
    s_files[fd].dir_sector = dir_sector;
    s_files[fd].dir_sector_offset = dir_sector_offset;
    memcpy(s_files[fd].dir_sector_buf, dir_sector_buffer, M65FTP_DIR_BUF_SIZE);
    s_files[fd].write_cluster = 0;
    s_files[fd].sector_in_cluster = 0;
    s_files[fd].in_use = 1;

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    pthread_mutex_unlock(&s_mutex);
    return fd;
}

int m65ftp_open_writable(const char *path)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }

    char dir_path[M65FTP_PATH_MAX];
    char file_name[M65FTP_NAME_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&s_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (fat_opendir(dir_path, 1)) { pthread_mutex_unlock(&s_mutex); return -1; }

    struct m65dirent dir_entry;
    while (fat_readdir(&dir_entry, 0) == 0) {
        if (dir_entry.d_name[0] == 0)
            continue;
        char *name = dir_entry.d_longname[0] ? dir_entry.d_longname : dir_entry.d_name;
        if (!strcmp(name, file_name)) {
            int fd = alloc_fd();
            if (fd < 0) { pthread_mutex_unlock(&s_mutex); return -1; }
            s_files[fd].first_cluster = dir_entry.d_ino;
            s_files[fd].file_size = dir_entry.d_filelen;
            s_files[fd].current_cluster = dir_entry.d_ino;
            s_files[fd].current_cluster_idx = 0;
            s_files[fd].last_offset = 0;
            s_files[fd].is_writable = 1;
            s_files[fd].buffered = 0;
            s_files[fd].write_buf = NULL;
            s_files[fd].write_buf_cap = 0;
            s_files[fd].dir_sector = dir_sector;
            s_files[fd].dir_sector_offset = dir_sector_offset;
            memcpy(s_files[fd].dir_sector_buf, dir_sector_buffer,
                   M65FTP_DIR_BUF_SIZE);
            s_files[fd].write_cluster = dir_entry.d_ino;
            s_files[fd].sector_in_cluster = 0;
            s_files[fd].in_use = 1;
            pthread_mutex_unlock(&s_mutex);
            return fd;
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return -1;
}

void m65ftp_close(int fd)
{
    pthread_mutex_lock(&s_mutex);
    if (fd < 0 || fd >= s_file_count || !s_files[fd].in_use) {
        pthread_mutex_unlock(&s_mutex);
        return;
    }

    if (s_files[fd].is_writable) {
        if (s_files[fd].buffered)
            flush_write_buffer(&s_files[fd]);
        if (s_files[fd].dir_sector >= 0 && s_files[fd].dir_sector_offset >= 0) {
            set_entry_file_size(&s_files[fd], s_files[fd].file_size);
            write_sector(partition_start + s_files[fd].dir_sector,
                         s_files[fd].dir_sector_buf);
        }
        execute_write_queue();
    }
    /* flush_write_buffer() only frees write_buf on success; free it here
       too so a failed flush can't leak the host-memory write buffer. */
    free(s_files[fd].write_buf);
    s_files[fd].write_buf = NULL;
    s_files[fd].write_buf_cap = 0;
    s_files[fd].first_cluster = 0;
    s_files[fd].in_use = 0;
    if (s_free_count < M65FTP_MAX_FILES)
        s_free_fds[s_free_count++] = fd;
    pthread_mutex_unlock(&s_mutex);
}

/* ---------------------------------------------------------------------
 * Read / write
 * ------------------------------------------------------------------- */

ssize_t m65ftp_pread(int fd, unsigned char *buf, size_t count, off_t offset)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized || fd < 0 || fd >= s_file_count || !s_files[fd].in_use) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    if (s_files[fd].buffered) {
        unsigned int file_size = s_files[fd].file_size;
        if (count == 0) { pthread_mutex_unlock(&s_mutex); return 0; }
        if ((off_t)offset >= (off_t)file_size) { pthread_mutex_unlock(&s_mutex); return 0; }
        if ((off_t)(offset + count) > (off_t)file_size)
            count = file_size - offset;
        size_t avail = s_files[fd].write_buf_cap;
        if ((size_t)offset < avail) {
            size_t from_buf = count;
            if ((size_t)offset + from_buf > avail)
                from_buf = avail - (size_t)offset;
            memcpy(buf, s_files[fd].write_buf + offset, from_buf);
            if (from_buf < count)
                memset(buf + from_buf, 0, count - from_buf);
        } else {
            memset(buf, 0, count);
        }
        pthread_mutex_unlock(&s_mutex);
        return (ssize_t)count;
    }

    unsigned int first_file_cluster = s_files[fd].first_cluster;
    unsigned int file_size = s_files[fd].file_size;

    if (count == 0 || first_file_cluster == 0) {
        pthread_mutex_unlock(&s_mutex);
        return 0;
    }
    if ((off_t)offset >= (off_t)file_size) { pthread_mutex_unlock(&s_mutex); return 0; }
    if ((off_t)(offset + count) > (off_t)file_size)
        count = file_size - offset;

    unsigned int cluster_size = sectors_per_cluster * 512;
    unsigned int first_data_sector = partition_start + reserved_sectors +
        2 * sectors_per_fat;

    unsigned int cluster = first_file_cluster;
    unsigned int cluster_index = 0;

    if (offset == s_files[fd].last_offset) {
        cluster = s_files[fd].current_cluster;
        cluster_index = s_files[fd].current_cluster_idx;
    }

    size_t total_read = 0;
    size_t remaining = count;
    off_t current_offset = offset;

    while (remaining > 0 && cluster < FAT32_MIN_END_OF_CLUSTER_MARKER) {
        unsigned int target_cluster_index =
            (unsigned int)(current_offset / cluster_size);
        unsigned int offset_in_cluster =
            (unsigned int)(current_offset % cluster_size);

        while (cluster_index < target_cluster_index) {
            cluster = get_next_cluster(cluster);
            cluster_index++;
            if (cluster >= FAT32_MIN_END_OF_CLUSTER_MARKER)
                break;
        }
        if (cluster >= FAT32_MIN_END_OF_CLUSTER_MARKER)
            break;

        unsigned int sector_in_cluster = offset_in_cluster / 512;
        unsigned int offset_in_sector = offset_in_cluster % 512;
        unsigned int sector_number = first_data_sector +
            (cluster - 2) * sectors_per_cluster + sector_in_cluster;

        size_t bytes_from_sector = 512 - offset_in_sector;
        if (bytes_from_sector > remaining)
            bytes_from_sector = remaining;

        unsigned char sector_buffer[M65FTP_DIR_BUF_SIZE];
        if (read_sector(sector_number, sector_buffer, CACHE_YES, 0)) {
            s_files[fd].current_cluster = cluster;
            s_files[fd].current_cluster_idx = cluster_index;
            s_files[fd].last_offset = current_offset;
            pthread_mutex_unlock(&s_mutex);
            return total_read > 0 ? (ssize_t)total_read : -1;
        }

        memcpy(buf + total_read, sector_buffer + offset_in_sector,
               bytes_from_sector);
        total_read += bytes_from_sector;
        current_offset += bytes_from_sector;
        remaining -= bytes_from_sector;
    }

    s_files[fd].current_cluster = cluster;
    s_files[fd].current_cluster_idx = cluster_index;
    s_files[fd].last_offset = current_offset;
    pthread_mutex_unlock(&s_mutex);
    return (ssize_t)total_read;
}

ssize_t m65ftp_pwrite(int fd, const unsigned char *buf, size_t count,
                      off_t offset)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized || fd < 0 || fd >= s_file_count ||
        !s_files[fd].in_use || !s_files[fd].is_writable) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    if (s_files[fd].buffered) {
        if (count == 0) { pthread_mutex_unlock(&s_mutex); return 0; }
        size_t end = (size_t)(offset + count);
        if (end > M65FTP_BUFFER_LIMIT) {
            /* Flush the current buffer, then fall through to direct SD
               writes for this request. */
            if (flush_write_buffer(&s_files[fd])) {
                pthread_mutex_unlock(&s_mutex);
                return -1;
            }
        } else {
            if (end > s_files[fd].write_buf_cap) {
                size_t new_cap = s_files[fd].write_buf_cap;
                if (new_cap == 0) new_cap = 4096;
                while (new_cap < end) new_cap *= 2;
                if (new_cap > M65FTP_BUFFER_LIMIT)
                    new_cap = M65FTP_BUFFER_LIMIT;
                unsigned char *new_buffer =
                    realloc(s_files[fd].write_buf, new_cap);
                if (!new_buffer) {
                    pthread_mutex_unlock(&s_mutex);
                    return -1;
                }
                memset(new_buffer + s_files[fd].write_buf_cap, 0,
                       new_cap - s_files[fd].write_buf_cap);
                s_files[fd].write_buf = new_buffer;
                s_files[fd].write_buf_cap = new_cap;
            }
            memcpy(s_files[fd].write_buf + offset, buf, count);
            if ((off_t)(offset + count) > (off_t)s_files[fd].file_size)
                s_files[fd].file_size = (unsigned int)(offset + count);
            pthread_mutex_unlock(&s_mutex);
            return (ssize_t)count;
        }
    }

    unsigned int first_file_cluster = s_files[fd].first_cluster;

    if (count == 0) { pthread_mutex_unlock(&s_mutex); return 0; }

    unsigned int cluster_size = sectors_per_cluster * 512;
    unsigned int first_data_sector = partition_start + reserved_sectors +
        2 * sectors_per_fat;

    size_t total_written = 0;
    size_t remaining = count;
    off_t current_offset = offset;

    /* Resolve the starting cluster for this write offset, reusing the
       sequential position when this call continues the previous one. */
    unsigned int cluster;
    int sector_in_cluster;
    if ((off_t)offset == s_files[fd].last_offset &&
        s_files[fd].last_offset > 0 && s_files[fd].write_cluster != 0) {
        cluster = s_files[fd].write_cluster;
        sector_in_cluster = s_files[fd].sector_in_cluster;
    } else {
        /* Walk the chain from the beginning to find the target cluster. */
        unsigned int cluster_index = (unsigned int)(current_offset / cluster_size);
        cluster = first_file_cluster;
        unsigned int chain_len = 0;
        for (unsigned int i = 0; i < cluster_index; i++) {
            unsigned int next = chained_cluster(cluster);
            if (next == 0 || next >= FAT32_MIN_END_OF_CLUSTER_MARKER)
                break;
            cluster = next;
            chain_len++;
        }
        if (chain_len < cluster_index) {
            /* Chain too short — force extension in the write loop. */
            sector_in_cluster = sectors_per_cluster;
        } else {
            sector_in_cluster = (unsigned int)(current_offset % cluster_size) / 512;
        }
    }

    while (remaining > 0) {
        if (sector_in_cluster >= sectors_per_cluster) {
            unsigned int next = chained_cluster(cluster);
            if (next == 0 || next >= FAT32_MIN_END_OF_CLUSTER_MARKER) {
                /* Try descending batch sizes for best contiguous allocation. */
                unsigned int remaining_clusters =
                    (unsigned int)((remaining + cluster_size - 1) / cluster_size) + 1;
                unsigned int try_batch = remaining_clusters;
                if (try_batch > 64) try_batch = 64;
                unsigned int batch_power = 1;
                while (batch_power <= try_batch) batch_power <<= 1;
                try_batch = batch_power >> 1;
                if (try_batch < 2) try_batch = 2;

                unsigned int batch_start = 0;
                while (try_batch >= 2) {
                    batch_start = find_contiguous_clusters(try_batch);
                    if (batch_start &&
                        batch_start < FAT32_MIN_END_OF_CLUSTER_MARKER)
                        break;
                    try_batch >>= 1;
                }
                if (batch_start && batch_start < FAT32_MIN_END_OF_CLUSTER_MARKER) {
                    allocate_cluster(batch_start);
                    chain_cluster(cluster, batch_start);
                    for (unsigned int i = 0; i < try_batch - 1; i++) {
                        unsigned int batch_cluster = batch_start + i;
                        unsigned int next_cluster = batch_cluster + 1;
                        allocate_cluster(next_cluster);
                        chain_cluster(batch_cluster, next_cluster);
                    }
                    next = batch_start;
                } else {
                    /* Fall back to single-cluster allocation. */
                    next = find_free_cluster(cluster);
                    if (!next || next >= FAT32_MIN_END_OF_CLUSTER_MARKER) {
                        if (total_written > 0) break;
                        pthread_mutex_unlock(&s_mutex);
                        return -1;
                    }
                    allocate_cluster(next);
                    chain_cluster(cluster, next);
                }
            }
            cluster = next;
            sector_in_cluster = 0;
        }

        unsigned int sector_number = first_data_sector +
            (cluster - 2) * sectors_per_cluster + sector_in_cluster;

        /* Full-sector writes go out directly; partial ones use a
           read-modify-write cycle. */
        unsigned char sector_buffer[M65FTP_DIR_BUF_SIZE];
        int offset_in_sector = (unsigned int)(current_offset % 512);
        size_t bytes_this_sector = 512 - offset_in_sector;
        if (bytes_this_sector > remaining)
            bytes_this_sector = remaining;

        if (bytes_this_sector < 512 || offset_in_sector != 0) {
            if (read_sector(sector_number, sector_buffer, CACHE_YES, 0) != 0) {
                if (total_written > 0) break;
                pthread_mutex_unlock(&s_mutex);
                return -1;
            }
        }

        memcpy(sector_buffer + offset_in_sector, buf + total_written,
               bytes_this_sector);

        if (write_sector(sector_number, sector_buffer)) {
            if (total_written > 0) break;
            pthread_mutex_unlock(&s_mutex);
            return -1;
        }

        total_written += bytes_this_sector;
        current_offset += bytes_this_sector;
        remaining -= bytes_this_sector;
        sector_in_cluster++;
    }

    s_files[fd].write_cluster = cluster;
    s_files[fd].sector_in_cluster = sector_in_cluster;
    s_files[fd].last_offset = current_offset;
    if ((off_t)(offset + total_written) > (off_t)s_files[fd].file_size)
        s_files[fd].file_size = (unsigned int)(offset + total_written);

    /* Write the updated file size directly to the per-fd cached dir entry
       (no globals). */
    if (s_files[fd].dir_sector >= 0 && s_files[fd].dir_sector_offset >= 0) {
        set_entry_file_size(&s_files[fd], s_files[fd].file_size);
        write_sector(partition_start + s_files[fd].dir_sector,
                     s_files[fd].dir_sector_buf);
    }

    pthread_mutex_unlock(&s_mutex);
    return (ssize_t)total_written;
}

int m65ftp_ftruncate(int fd, off_t length)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized || fd < 0 || fd >= s_file_count ||
        !s_files[fd].in_use || !s_files[fd].is_writable) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    if (length < 0) { pthread_mutex_unlock(&s_mutex); return -1; }

    if (s_files[fd].buffered) {
        if (length == 0) {
            free(s_files[fd].write_buf);
            s_files[fd].write_buf = NULL;
            s_files[fd].write_buf_cap = 0;
            s_files[fd].first_cluster = 0;
            s_files[fd].file_size = 0;
            s_files[fd].write_cluster = 0;
            s_files[fd].sector_in_cluster = 0;
            s_files[fd].last_offset = 0;
            s_files[fd].current_cluster = 0;
            s_files[fd].current_cluster_idx = 0;
            if (s_files[fd].dir_sector >= 0 && s_files[fd].dir_sector_offset >= 0) {
                set_entry_first_cluster(&s_files[fd], 0);
                set_entry_file_size(&s_files[fd], 0);
                write_sector(partition_start + s_files[fd].dir_sector,
                             s_files[fd].dir_sector_buf);
                execute_write_queue();
            }
            pthread_mutex_unlock(&s_mutex);
            return 0;
        }
        s_files[fd].file_size = (unsigned int)length;
        if (s_files[fd].write_buf_cap < (size_t)length) {
            size_t new_cap = s_files[fd].write_buf_cap;
            if (new_cap == 0) new_cap = 4096;
            while (new_cap < (size_t)length) new_cap *= 2;
            if (new_cap > M65FTP_BUFFER_LIMIT) new_cap = M65FTP_BUFFER_LIMIT;
            unsigned char *new_buffer = realloc(s_files[fd].write_buf, new_cap);
            if (!new_buffer) {
                pthread_mutex_unlock(&s_mutex);
                return -1;
            }
            memset(new_buffer + s_files[fd].write_buf_cap, 0,
                   new_cap - s_files[fd].write_buf_cap);
            s_files[fd].write_buf = new_buffer;
            s_files[fd].write_buf_cap = new_cap;
        }
        pthread_mutex_unlock(&s_mutex);
        return 0;
    }

    unsigned int cluster_size = sectors_per_cluster * 512;
    unsigned int needed_clusters = length == 0
        ? 0
        : (unsigned int)(((unsigned long long)length + cluster_size - 1) / cluster_size);

    /* Truncate to zero → free everything (fast path, also handles a
       zero-length file with no clusters). */
    if (needed_clusters == 0) {
        unsigned int cluster = s_files[fd].first_cluster;
        while (cluster > 0 && cluster < FAT32_MIN_END_OF_CLUSTER_MARKER) {
            unsigned int next = chained_cluster(cluster);
            deallocate_cluster(cluster);
            cluster = next;
        }

        if (s_files[fd].dir_sector >= 0 && s_files[fd].dir_sector_offset >= 0) {
            set_entry_first_cluster(&s_files[fd], 0);
            set_entry_file_size(&s_files[fd], 0);
            write_sector(partition_start + s_files[fd].dir_sector,
                         s_files[fd].dir_sector_buf);
            execute_write_queue();
        }

        s_files[fd].first_cluster = 0;
        s_files[fd].file_size = 0;
        s_files[fd].write_cluster = 0;
        s_files[fd].sector_in_cluster = 0;
        s_files[fd].last_offset = 0;
        s_files[fd].current_cluster = 0;
        s_files[fd].current_cluster_idx = 0;

        pthread_mutex_unlock(&s_mutex);
        return 0;
    }

    /* Non-zero truncation. */
    unsigned int first = s_files[fd].first_cluster;
    if (first == 0 || first >= FAT32_MIN_END_OF_CLUSTER_MARKER) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    unsigned int last;
    unsigned int current_count = count_cluster_chain(first, &last);

    if (needed_clusters > current_count) {
        /* Grow: build a brand-new contiguous block and copy the old data
           into it before touching the old chain or the directory entry, so
           a failure at any point before the swap leaves the file, and the
           on-disk directory entry, completely untouched. */
        unsigned int start = find_contiguous_clusters(needed_clusters);
        if (!start || start >= FAT32_MIN_END_OF_CLUSTER_MARKER) {
            pthread_mutex_unlock(&s_mutex);
            return -1;
        }

        /* Allocate and chain every cluster of the new block first.
           allocated_count always reflects exactly how many clusters from
           start are actually marked used, whether allocation or chaining
           is what failed, so the rollback below never leaks or double-frees
           a cluster. */
        unsigned int allocated_count = 0;
        int growth_failed = 0;
        for (unsigned int i = 0; i < needed_clusters; i++) {
            unsigned int cluster = start + i;
            if (allocate_cluster(cluster)) {
                growth_failed = 1;
                break;
            }
            allocated_count = i + 1;
            if (i > 0 && chain_cluster(start + i - 1, cluster)) {
                growth_failed = 1;
                break;
            }
        }
        if (growth_failed) {
            for (unsigned int i = 0; i < allocated_count; i++)
                deallocate_cluster(start + i);
            pthread_mutex_unlock(&s_mutex);
            return -1;
        }
        last = start + needed_clusters - 1;

        /* Copy existing data from the old clusters to the new ones. */
        unsigned int copy_size = s_files[fd].file_size;
        if (copy_size > (unsigned int)length)
            copy_size = (unsigned int)length;
        if (copy_size > 0) {
            unsigned int first_data_sector = partition_start + reserved_sectors +
                2 * sectors_per_fat;
            unsigned char buffer[M65FTP_DIR_BUF_SIZE];
            unsigned int old_cluster = first;
            unsigned int new_cluster = start;
            unsigned int remaining_bytes = copy_size;
            while (remaining_bytes > 0) {
                unsigned int sector_in_cluster = 0;
                while (sector_in_cluster < sectors_per_cluster &&
                       remaining_bytes > 0) {
                    unsigned int old_sector = first_data_sector +
                        (old_cluster - 2) * sectors_per_cluster + sector_in_cluster;
                    unsigned int new_sector = first_data_sector +
                        (new_cluster - 2) * sectors_per_cluster + sector_in_cluster;
                    if (read_sector(old_sector, buffer, CACHE_YES, 0) ||
                        write_sector(new_sector, buffer)) {
                        /* The old chain is still intact and still
                           referenced by the directory entry; just free the
                           unused new chain and bail out. */
                        for (unsigned int i = 0; i < needed_clusters; i++)
                            deallocate_cluster(start + i);
                        pthread_mutex_unlock(&s_mutex);
                        return -1;
                    }
                    remaining_bytes -= (remaining_bytes >= 512) ? 512 : remaining_bytes;
                    sector_in_cluster++;
                }
                if (remaining_bytes > 0) {
                    unsigned int next_old = chained_cluster(old_cluster);
                    if (next_old == 0 || next_old >= FAT32_MIN_END_OF_CLUSTER_MARKER)
                        break;
                    old_cluster = next_old;
                    new_cluster++;
                }
            }
        }

        /* The new chain is complete and holds the data: now it is safe to
           free the old chain and switch the directory entry over. */
        {
            unsigned int cluster = first;
            while (cluster > 0 && cluster < FAT32_MIN_END_OF_CLUSTER_MARKER) {
                unsigned int next = chained_cluster(cluster);
                deallocate_cluster(cluster);
                cluster = next;
            }
        }

        if (s_files[fd].dir_sector >= 0 && s_files[fd].dir_sector_offset >= 0)
            set_entry_first_cluster(&s_files[fd], start);

        first = start;
    } else if (needed_clusters < current_count) {
        /* Shrink: walk to the new last cluster, free everything after it. */
        unsigned int cluster = first;
        for (unsigned int i = 1; i < needed_clusters; i++) {
            unsigned int next = chained_cluster(cluster);
            if (next == 0 || next >= FAT32_MIN_END_OF_CLUSTER_MARKER)
                break;
            cluster = next;
        }
        unsigned int free_start = chained_cluster(cluster);
        while (free_start > 0 && free_start < FAT32_MIN_END_OF_CLUSTER_MARKER) {
            unsigned int next = chained_cluster(free_start);
            deallocate_cluster(free_start);
            free_start = next;
        }
        set_fat_cluster_ptr(cluster, FAT32_MIN_END_OF_CLUSTER_MARKER);
        last = cluster;
    }

    /* Update file size in the per-fd dir entry cache and write the sector. */
    if (s_files[fd].dir_sector >= 0 && s_files[fd].dir_sector_offset >= 0) {
        set_entry_file_size(&s_files[fd], (unsigned int)length);
        write_sector(partition_start + s_files[fd].dir_sector,
                     s_files[fd].dir_sector_buf);
        execute_write_queue();
    }
    s_files[fd].file_size = (unsigned int)length;

    /* Persist the first_cluster change if we re-allocated. */
    s_files[fd].first_cluster = first;

    /* Reset write tracking to the start of the file. */
    s_files[fd].write_cluster = first;
    s_files[fd].sector_in_cluster = 0;
    s_files[fd].last_offset = 0;
    s_files[fd].current_cluster = first;
    s_files[fd].current_cluster_idx = 0;

    pthread_mutex_unlock(&s_mutex);
    return 0;
}

int m65ftp_fsync(int fd)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized || fd < 0 || fd >= s_file_count || !s_files[fd].in_use) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    if (s_files[fd].is_writable && s_files[fd].buffered) {
        if (flush_write_buffer(&s_files[fd])) {
            pthread_mutex_unlock(&s_mutex);
            return -1;
        }
    }
    execute_write_queue();
    pthread_mutex_unlock(&s_mutex);
    return 0;
}

int m65ftp_sync(void)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }
    execute_write_queue();
    pthread_mutex_unlock(&s_mutex);
    return 0;
}

/* ---------------------------------------------------------------------
 * Metadata
 * ------------------------------------------------------------------- */

int m65ftp_stat(const char *path, unsigned int *file_size, int *is_dir,
                time_t *mtime)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }

    if (path[0] == '/' && path[1] == 0) {
        if (file_size) *file_size = 0;
        if (is_dir) *is_dir = 1;
        if (mtime) *mtime = time(NULL);
        pthread_mutex_unlock(&s_mutex);
        return 0;
    }

    char dir_path[M65FTP_PATH_MAX];
    char file_name[M65FTP_NAME_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&s_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (fat_opendir(dir_path, 0)) { pthread_mutex_unlock(&s_mutex); return -1; }

    struct m65dirent dir_entry;
    while (fat_readdir(&dir_entry, 0) == 0) {
        if (dir_entry.d_name[0] == 0)
            continue;
        char *name = dir_entry.d_longname[0] ? dir_entry.d_longname : dir_entry.d_name;
        if (!strcmp(name, file_name)) {
            if (file_size) *file_size = dir_entry.d_filelen;
            if (is_dir) *is_dir = (dir_entry.d_attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
            if (mtime) *mtime = mktime(&dir_entry.d_mtime);
            pthread_mutex_unlock(&s_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return -1;
}

int m65ftp_readdir(const char *path,
                   int (*cb)(const char *name, unsigned int file_size,
                             int is_dir, time_t mtime, void *ctx),
                   void *ctx)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }

    if (fat_opendir((char *)path, 0)) { pthread_mutex_unlock(&s_mutex); return -1; }

    struct m65dirent dir_entry;
    while (fat_readdir(&dir_entry, 0) == 0) {
        if (dir_entry.d_name[0] == 0)
            continue;
        char *name = dir_entry.d_longname[0] ? dir_entry.d_longname : dir_entry.d_name;
        int is_dir = (dir_entry.d_attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
        time_t mtime = mktime(&dir_entry.d_mtime);
        if (cb(name, dir_entry.d_filelen, is_dir, mtime, ctx))
            break;
    }
    pthread_mutex_unlock(&s_mutex);
    return 0;
}

int m65ftp_utimens(const char *path, const struct timespec tv[2])
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }
    if (!path || !path[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    if (strcmp(path, "/") == 0) { pthread_mutex_unlock(&s_mutex); return -1; }

    char dir_path[M65FTP_PATH_MAX];
    char file_name[M65FTP_NAME_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&s_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!file_name[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    time_t now = time(NULL);
    time_t atime, mtime;
    int set_atime = 1, set_mtime = 1;

    if (tv) {
        if (tv[0].tv_nsec == UTIME_OMIT)
            set_atime = 0;
        else if (tv[0].tv_nsec == UTIME_NOW)
            atime = now;
        else
            atime = tv[0].tv_sec;

        if (tv[1].tv_nsec == UTIME_OMIT)
            set_mtime = 0;
        else if (tv[1].tv_nsec == UTIME_NOW)
            mtime = now;
        else
            mtime = tv[1].tv_sec;
    } else {
        atime = now;
        mtime = now;
    }

    char saved_dir[M65FTP_PATH_MAX];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    struct m65dirent dir_entry;
    if (!find_file_in_curdir(file_name, &dir_entry)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    if (set_mtime) {
        struct tm *tm = localtime(&mtime);
        if (tm) {
            uint16_t time_word = (tm->tm_hour << 11) | (tm->tm_min << 5) |
                                 (tm->tm_sec >> 1);
            uint16_t date_word = ((tm->tm_year - 80) << 9) |
                                 ((tm->tm_mon + 1) << 5) | tm->tm_mday;
            dir_sector_buffer[dir_sector_offset + DIR_ENTRY_WRITE_TIME] =
                time_word & 0xFF;
            dir_sector_buffer[dir_sector_offset + DIR_ENTRY_WRITE_TIME + 1] =
                (time_word >> 8) & 0xFF;
            dir_sector_buffer[dir_sector_offset + DIR_ENTRY_WRITE_DATE] =
                date_word & 0xFF;
            dir_sector_buffer[dir_sector_offset + DIR_ENTRY_WRITE_DATE + 1] =
                (date_word >> 8) & 0xFF;
        }
    }

    if (set_atime) {
        struct tm *tm = localtime(&atime);
        if (tm) {
            uint16_t date_word = ((tm->tm_year - 80) << 9) |
                                 ((tm->tm_mon + 1) << 5) | tm->tm_mday;
            dir_sector_buffer[dir_sector_offset + DIR_ENTRY_ACCESS_DATE] =
                date_word & 0xFF;
            dir_sector_buffer[dir_sector_offset + DIR_ENTRY_ACCESS_DATE + 1] =
                (date_word >> 8) & 0xFF;
        }
    }

    int ret = write_sector(partition_start + dir_sector, dir_sector_buffer);
    if (!ret)
        execute_write_queue();

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    pthread_mutex_unlock(&s_mutex);
    return ret ? -1 : 0;
}

/* ---------------------------------------------------------------------
 * Directory operations
 * ------------------------------------------------------------------- */

int m65ftp_mkdir(const char *path)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }
    if (!path || !path[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    char dir_path[M65FTP_PATH_MAX];
    char dir_name[M65FTP_NAME_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&s_mutex); return -1; }

    strncpy(dir_name, last_slash + 1, sizeof(dir_name) - 1);
    dir_name[sizeof(dir_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!dir_name[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    char saved_dir[M65FTP_PATH_MAX];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    int ret = create_dir(dir_name);

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    pthread_mutex_unlock(&s_mutex);
    return ret ? -1 : 0;
}

int m65ftp_rmdir(const char *path)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }
    if (!path || !path[0] || strcmp(path, "/") == 0) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    char dir_path[M65FTP_PATH_MAX];
    char dir_name[M65FTP_NAME_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&s_mutex); return -1; }

    strncpy(dir_name, last_slash + 1, sizeof(dir_name) - 1);
    dir_name[sizeof(dir_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!dir_name[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    char saved_dir[M65FTP_PATH_MAX];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    /* Verify the target is a directory. */
    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    if (fat_opendir(dir_path, 0)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }
    struct m65dirent dir_entry;
    if (!find_file_in_curdir(dir_name, &dir_entry)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }
    if (!(dir_entry.d_attr & FAT_ATTR_DIRECTORY)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    /* Check that the directory is empty. */
    char full_path[M65FTP_PATH_MAX];
    if (strcmp(dir_path, "/") == 0)
        snprintf(full_path, sizeof(full_path), "/%s", dir_name);
    else
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dir_name);

    strncpy(current_dir, full_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    if (fat_opendir(full_path, 0)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    struct m65dirent entry;
    while (fat_readdir(&entry, 0) == 0) {
        if (entry.d_name[0] == 0) continue;
        char *name = entry.d_longname[0] ? entry.d_longname : entry.d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        /* Directory is not empty. */
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    /* Switch back to the parent directory and delete. */
    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    int ret = delete_single_file(dir_name, TRUE);

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;
    pthread_mutex_unlock(&s_mutex);
    return ret ? -1 : 0;
}

int m65ftp_unlink(const char *path)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }
    if (!path || !path[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    char dir_path[M65FTP_PATH_MAX];
    char file_name[M65FTP_NAME_MAX];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&s_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!file_name[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    char saved_dir[M65FTP_PATH_MAX];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    /* Refuse to delete directories. */
    if (fat_opendir(dir_path, 0)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }
    struct m65dirent dir_entry;
    if (!find_file_in_curdir(file_name, &dir_entry)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }
    if (dir_entry.d_attr & FAT_ATTR_DIRECTORY) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    int ret = delete_single_file(file_name, TRUE);

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    pthread_mutex_unlock(&s_mutex);
    return ret ? -1 : 0;
}

int m65ftp_rename(const char *oldpath, const char *newpath)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_initialized) { pthread_mutex_unlock(&s_mutex); return -1; }
    if (!oldpath || !oldpath[0] || !newpath || !newpath[0]) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    char old_dir[M65FTP_PATH_MAX], old_name[M65FTP_NAME_MAX];
    strncpy(old_dir, oldpath, sizeof(old_dir) - 1);
    old_dir[sizeof(old_dir) - 1] = 0;
    char *last = strrchr(old_dir, '/');
    if (!last) { pthread_mutex_unlock(&s_mutex); return -1; }
    strncpy(old_name, last + 1, sizeof(old_name) - 1);
    old_name[sizeof(old_name) - 1] = 0;
    if (last == old_dir) *(last + 1) = 0; else *last = 0;

    char new_dir[M65FTP_PATH_MAX], new_name[M65FTP_NAME_MAX];
    strncpy(new_dir, newpath, sizeof(new_dir) - 1);
    new_dir[sizeof(new_dir) - 1] = 0;
    last = strrchr(new_dir, '/');
    if (!last) { pthread_mutex_unlock(&s_mutex); return -1; }
    strncpy(new_name, last + 1, sizeof(new_name) - 1);
    new_name[sizeof(new_name) - 1] = 0;
    if (last == new_dir) *(last + 1) = 0; else *last = 0;

    if (!old_name[0] || !new_name[0]) { pthread_mutex_unlock(&s_mutex); return -1; }

    if (strcmp(old_dir, new_dir) != 0) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    char saved_dir[M65FTP_PATH_MAX];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, old_dir, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    int ret = rename_file_or_dir(old_name, new_name);

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;
    pthread_mutex_unlock(&s_mutex);
    return ret ? -1 : 0;
}
