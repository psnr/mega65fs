#define M65FTP_AS_LIB

#include "../lib/libm65ftp/mega65_ftp.c"

struct m65ftp_partition {
    unsigned int start_sector;
    unsigned int sector_count;
    unsigned char type;
};

#include <stddef.h>
#include <pthread.h>
#include <sys/stat.h>

static pthread_mutex_t g_m65ftp_mutex = PTHREAD_MUTEX_INITIALIZER;
static int m65ftp_initialized = 0;

int m65ftp_init(void)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (m65ftp_initialized) {
        pthread_mutex_unlock(&g_m65ftp_mutex);
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
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    ethl_setup_dmaload();

    if (trigger_eth_hyperrupt() < 0) {
        etherload_finish();
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    if (ethl_ping(3000) < 0) {
        log_error("No response from MEGA65");
        log_error("Please make sure ETHLOAD.M65 is available in the root folder of the SD card.");
        etherload_finish();
        pthread_mutex_unlock(&g_m65ftp_mutex);
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
            pthread_mutex_unlock(&g_m65ftp_mutex);
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

    if (ethl_single_command((uint8_t *)ethlet_all_done_basic2, ethlet_all_done_basic2_len, 2000) < 0) {
        log_error("No response from MEGA65");
        etherload_finish();
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    sockfd = ethl_get_socket();
    ethl_setup_callbacks(&ethernet_get_packet_seq, &ethernet_match_payloads,
        &ethernet_is_duplicate, &ethernet_embed_packet_seq, ethernet_timeout_handler);

    ethernet_login();
    log_info("Login successful");

    determine_ethernet_window_size();
    sdhc_check();

    if (!file_system_found)
        open_file_system();

    m65ftp_initialized = 1;
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return 0;
}

void m65ftp_finish(void)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (m65ftp_initialized) {
        request_quit();
        etherload_finish();
        m65ftp_initialized = 0;
    }
    pthread_mutex_unlock(&g_m65ftp_mutex);
}

int m65ftp_is_connected(void)
{
    int r;
    pthread_mutex_lock(&g_m65ftp_mutex);
    r = m65ftp_initialized;
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return r;
}

#define M65FTP_MAX_FILES 4096
#define M65FTP_BUFFER_LIMIT (64UL * 1024 * 1024)

struct m65ftp_fileinfo {
    unsigned int first_cluster;
    unsigned int file_size;
    unsigned int current_cluster;      // last resolved cluster for sequential continuation
    unsigned int current_cluster_idx;  // logical cluster index of current_cluster
    off_t last_offset;                 // byte offset where the last pread ended
    /* Write state */
    int is_writable;
    int dir_sector;                    // sector of directory entry
    int dir_sector_offset;             // offset within dir_sector
    unsigned char dir_sector_buf[512]; // cached copy of dir sector for finalising
    int buffered;                      // 1 = use host-memory writeback buffering
    unsigned char *write_buf;          // host-memory write buffer (NULL = not allocated)
    size_t write_buf_cap;              // allocated capacity of write_buf
    unsigned int write_cluster;        // current cluster being written to
    int sector_in_cluster;             // sector index within write_cluster
};

static int m65ftp_file_count = 0;
static struct m65ftp_fileinfo m65ftp_files[M65FTP_MAX_FILES];

/* Flush a buffered file's write_buf to a contiguous SD cluster chain.
   Must be called with g_m65ftp_mutex held.  On success the buffer is freed
   and the fd switches to direct (non-buffered) mode with first_cluster set. */
static int flush_buffer_to_sd(struct m65ftp_fileinfo *fi)
{
    if (fi->file_size == 0) {
        if (fi->dir_sector >= 0 && fi->dir_sector_offset >= 0) {
            unsigned int off = (unsigned int)fi->dir_sector_offset;
            fi->dir_sector_buf[off + 0x1A] = 0;
            fi->dir_sector_buf[off + 0x1B] = 0;
            fi->dir_sector_buf[off + 0x14] = 0;
            fi->dir_sector_buf[off + 0x15] = 0;
            fi->dir_sector_buf[off + 0x1C] = 0;
            fi->dir_sector_buf[off + 0x1D] = 0;
            fi->dir_sector_buf[off + 0x1E] = 0;
            fi->dir_sector_buf[off + 0x1F] = 0;
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

    /* Allocate and chain the contiguous block */
    allocate_cluster(start);
    for (unsigned int i = 1; i < needed; i++) {
        unsigned int c = start + i;
        allocate_cluster(c);
        chain_cluster(start + i - 1, c);
    }

    /* Write data sectors (zero-fill gaps beyond buffer, e.g. sparse ftruncate) */
    unsigned int first_data_sector = partition_start + reserved_sectors + 2 * sectors_per_fat;
    unsigned char sector_buf[512];
    size_t remaining = fi->file_size;
    unsigned int cluster = start;
    while (remaining > 0) {
        for (unsigned int sec = 0; sec < sectors_per_cluster && remaining > 0; sec++) {
            unsigned int sector_num = first_data_sector + (cluster - 2) * sectors_per_cluster + sec;
            size_t off = (size_t)(cluster - start) * cluster_size + sec * 512;
            size_t copy = 512;
            if (copy > remaining) copy = remaining;
            memset(sector_buf, 0, 512);
            if (fi->write_buf && off < fi->write_buf_cap) {
                size_t buf_copy = copy;
                if (off + buf_copy > fi->write_buf_cap)
                    buf_copy = fi->write_buf_cap - off;
                memcpy(sector_buf, fi->write_buf + off, buf_copy);
            }
            if (write_sector(sector_num, sector_buf))
                return -1;
            remaining -= copy;
        }
        cluster++;
    }

    /* Update directory entry with new first_cluster and file_size */
    if (fi->dir_sector >= 0 && fi->dir_sector_offset >= 0) {
        unsigned int off = (unsigned int)fi->dir_sector_offset;
        fi->dir_sector_buf[off + 0x1A] = start & 0xff;
        fi->dir_sector_buf[off + 0x1B] = (start >> 8) & 0xff;
        fi->dir_sector_buf[off + 0x14] = (start >> 16) & 0xff;
        fi->dir_sector_buf[off + 0x15] = (start >> 24) & 0xff;
        fi->dir_sector_buf[off + 0x1C] = fi->file_size & 0xff;
        fi->dir_sector_buf[off + 0x1D] = (fi->file_size >> 8) & 0xff;
        fi->dir_sector_buf[off + 0x1E] = (fi->file_size >> 16) & 0xff;
        fi->dir_sector_buf[off + 0x1F] = (fi->file_size >> 24) & 0xff;
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

int m65ftp_open(const char *path)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    if (m65ftp_file_count >= M65FTP_MAX_FILES) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char dir_path[1024];
    char file_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (fat_opendir(dir_path, 0)) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    struct m65dirent de;
    while (fat_readdir(&de, 0) == 0) {
        if (de.d_name[0] == 0)
            continue;
        char *name = de.d_longname[0] ? de.d_longname : de.d_name;
        if (!strcmp(name, file_name)) {
            int idx = m65ftp_file_count++;
            m65ftp_files[idx].first_cluster = de.d_ino;
            m65ftp_files[idx].file_size = de.d_filelen;
            m65ftp_files[idx].current_cluster = de.d_ino;
            m65ftp_files[idx].current_cluster_idx = 0;
            m65ftp_files[idx].last_offset = 0;
            m65ftp_files[idx].is_writable = 0;
            m65ftp_files[idx].buffered = 0;
            m65ftp_files[idx].write_buf = NULL;
            m65ftp_files[idx].write_buf_cap = 0;
            memset(m65ftp_files[idx].dir_sector_buf, 0, 512);
            m65ftp_files[idx].dir_sector = -1;
            m65ftp_files[idx].write_cluster = 0;
            m65ftp_files[idx].sector_in_cluster = 0;
            pthread_mutex_unlock(&g_m65ftp_mutex);
            return idx;
        }
    }
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return -1;
}

void m65ftp_close(int fd)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (fd < 0 || fd >= m65ftp_file_count) { pthread_mutex_unlock(&g_m65ftp_mutex); return; }

    if (m65ftp_files[fd].is_writable) {
        if (m65ftp_files[fd].buffered)
            flush_buffer_to_sd(&m65ftp_files[fd]);
        if (m65ftp_files[fd].dir_sector >= 0 && m65ftp_files[fd].dir_sector_offset >= 0) {
            unsigned int off = (unsigned int)m65ftp_files[fd].dir_sector_offset;
            m65ftp_files[fd].dir_sector_buf[off + 0x1C] = (m65ftp_files[fd].file_size >> 0) & 0xff;
            m65ftp_files[fd].dir_sector_buf[off + 0x1D] = (m65ftp_files[fd].file_size >> 8) & 0xff;
            m65ftp_files[fd].dir_sector_buf[off + 0x1E] = (m65ftp_files[fd].file_size >> 16) & 0xff;
            m65ftp_files[fd].dir_sector_buf[off + 0x1F] = (m65ftp_files[fd].file_size >> 24) & 0xff;
            write_sector(partition_start + m65ftp_files[fd].dir_sector, m65ftp_files[fd].dir_sector_buf);
        }
        execute_write_queue();
    }
    m65ftp_files[fd].first_cluster = 0;
    pthread_mutex_unlock(&g_m65ftp_mutex);
}

int m65ftp_create(const char *path)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    if (m65ftp_file_count >= M65FTP_MAX_FILES) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char dir_path[1024];
    char file_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!file_name[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char saved_dir[1024];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    if (fat_opendir(dir_path, 1)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    struct m65dirent de;
    if (find_file_in_curdir(file_name, &de)) {
        delete_file_or_dir(file_name);
        fat_opendir(dir_path, 1);
    }

    if (!create_direntry_with_attrib(file_name, DE_ATTRIB_FILE)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }
    /* Persist directory entry with first_cluster = 0 (clusters allocated on close) */
    write_sector(partition_start + dir_sector, dir_sector_buffer);

    int idx = m65ftp_file_count++;
    m65ftp_files[idx].first_cluster = 0;
    m65ftp_files[idx].file_size = 0;
    m65ftp_files[idx].current_cluster = 0;
    m65ftp_files[idx].current_cluster_idx = 0;
    m65ftp_files[idx].last_offset = 0;
    m65ftp_files[idx].is_writable = 1;
    m65ftp_files[idx].buffered = 1;
    m65ftp_files[idx].write_buf = NULL;
    m65ftp_files[idx].write_buf_cap = 0;
    m65ftp_files[idx].dir_sector = dir_sector;
    m65ftp_files[idx].dir_sector_offset = dir_sector_offset;
    memcpy(m65ftp_files[idx].dir_sector_buf, dir_sector_buffer, 512);
    m65ftp_files[idx].write_cluster = 0;
    m65ftp_files[idx].sector_in_cluster = 0;

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return idx;
}

ssize_t m65ftp_pwrite(int fd, const unsigned char *buf, size_t count, off_t offset)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized || fd < 0 || fd >= m65ftp_file_count || !m65ftp_files[fd].is_writable) {
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    if (m65ftp_files[fd].buffered) {
        if (count == 0) { pthread_mutex_unlock(&g_m65ftp_mutex); return 0; }
        size_t end = (size_t)(offset + count);
        if (end > M65FTP_BUFFER_LIMIT) {
            /* Exceeds buffer limit — flush current buffer then fall through to direct SD writes */
            if (flush_buffer_to_sd(&m65ftp_files[fd])) {
                pthread_mutex_unlock(&g_m65ftp_mutex);
                return -1;
            }
            /* Fall through to the normal pwrite path below */
        } else {
            /* Ensure buffer is large enough */
            if (end > m65ftp_files[fd].write_buf_cap) {
                size_t new_cap = m65ftp_files[fd].write_buf_cap;
                if (new_cap == 0) new_cap = 4096;
                while (new_cap < end) new_cap *= 2;
                if (new_cap > M65FTP_BUFFER_LIMIT) new_cap = M65FTP_BUFFER_LIMIT;
                unsigned char *nb = realloc(m65ftp_files[fd].write_buf, new_cap);
                if (!nb) {
                    pthread_mutex_unlock(&g_m65ftp_mutex);
                    return -1;
                }
                memset(nb + m65ftp_files[fd].write_buf_cap, 0, new_cap - m65ftp_files[fd].write_buf_cap);
                m65ftp_files[fd].write_buf = nb;
                m65ftp_files[fd].write_buf_cap = new_cap;
            }
            memcpy(m65ftp_files[fd].write_buf + offset, buf, count);
            if ((off_t)(offset + count) > (off_t)m65ftp_files[fd].file_size)
                m65ftp_files[fd].file_size = (unsigned int)(offset + count);
            pthread_mutex_unlock(&g_m65ftp_mutex);
            return (ssize_t)count;
        }
    }

    unsigned int first_file_cluster = m65ftp_files[fd].first_cluster;

    if (count == 0) { pthread_mutex_unlock(&g_m65ftp_mutex); return 0; }

    unsigned int cluster_size = sectors_per_cluster * 512;
    unsigned int first_data_sector = partition_start + reserved_sectors + 2 * sectors_per_fat;

    size_t total_written = 0;
    size_t remaining = count;
    off_t current_offset = offset;

    /* Resolve starting cluster for this write offset */
    unsigned int cluster;
    int sector_in_cluster;
    if ((off_t)offset == m65ftp_files[fd].last_offset && m65ftp_files[fd].last_offset > 0 &&
        m65ftp_files[fd].write_cluster != 0) {
        cluster = m65ftp_files[fd].write_cluster;
        sector_in_cluster = m65ftp_files[fd].sector_in_cluster;
    } else {
        /* Walk the cluster chain from the beginning to find the target cluster */
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
            /* Chain too short — force extension in the write loop */
            sector_in_cluster = sectors_per_cluster;
        } else {
            sector_in_cluster = (unsigned int)(current_offset % cluster_size) / 512;
        }
    }

    while (remaining > 0) {
        if (sector_in_cluster >= sectors_per_cluster) {
            unsigned int next = chained_cluster(cluster);
            if (next == 0 || next >= FAT32_MIN_END_OF_CLUSTER_MARKER) {
                /* Try descending batch sizes for best contiguous allocation */
                unsigned int remaining_clusters = (unsigned int)((remaining + cluster_size - 1) / cluster_size) + 1;
                unsigned int try_batch = remaining_clusters;
                if (try_batch > 64) try_batch = 64;
                unsigned int p = 1;
                while (p <= try_batch) p <<= 1;
                try_batch = p >> 1;
                if (try_batch < 2) try_batch = 2;

                unsigned int batch_start = 0;
                while (try_batch >= 2) {
                    batch_start = find_contiguous_clusters(try_batch);
                    if (batch_start && batch_start < FAT32_MIN_END_OF_CLUSTER_MARKER)
                        break;
                    try_batch >>= 1;
                }
                if (batch_start && batch_start < FAT32_MIN_END_OF_CLUSTER_MARKER) {
                    allocate_cluster(batch_start);
                    chain_cluster(cluster, batch_start);
                    for (unsigned int i = 0; i < try_batch - 1; i++) {
                        unsigned int b = batch_start + i;
                        unsigned int bn = b + 1;
                        allocate_cluster(bn);
                        chain_cluster(b, bn);
                    }
                    next = batch_start;
                } else {
                    /* Fall back to single cluster allocation */
                    next = find_free_cluster(cluster);
                    if (!next || next >= FAT32_MIN_END_OF_CLUSTER_MARKER) {
                        if (total_written > 0) break;
                        pthread_mutex_unlock(&g_m65ftp_mutex);
                        return -1;
                    }
                    allocate_cluster(next);
                    chain_cluster(cluster, next);
                }
            }
            cluster = next;
            sector_in_cluster = 0;
        }

        unsigned int sector_num = first_data_sector + (cluster - 2) * sectors_per_cluster + sector_in_cluster;

        /* For full-sector writes, write directly. For partial, read-modify-write */
        unsigned char sector_buf[512];
        int offset_in_sector = (unsigned int)(current_offset % 512);
        size_t bytes_this_sector = 512 - offset_in_sector;
        if (bytes_this_sector > remaining)
            bytes_this_sector = remaining;

        if (bytes_this_sector < 512 || offset_in_sector != 0) {
            if (read_sector(sector_num, sector_buf, CACHE_YES, 0) != 0) {
                if (total_written > 0) break;
                pthread_mutex_unlock(&g_m65ftp_mutex);
                return -1;
            }
        }

        memcpy(sector_buf + offset_in_sector, buf + total_written, bytes_this_sector);

        if (write_sector(sector_num, sector_buf)) {
            if (total_written > 0) break;
            pthread_mutex_unlock(&g_m65ftp_mutex);
            return -1;
        }

        total_written += bytes_this_sector;
        current_offset += bytes_this_sector;
        remaining -= bytes_this_sector;
        sector_in_cluster++;
    }

    m65ftp_files[fd].write_cluster = cluster;
    m65ftp_files[fd].sector_in_cluster = sector_in_cluster;
    m65ftp_files[fd].last_offset = current_offset;
    if ((off_t)(offset + total_written) > (off_t)m65ftp_files[fd].file_size)
        m65ftp_files[fd].file_size = (unsigned int)(offset + total_written);

    /* Write updated file size directly to per-fd cached dir entry (no globals) */
    if (m65ftp_files[fd].dir_sector >= 0 && m65ftp_files[fd].dir_sector_offset >= 0) {
        unsigned int off = (unsigned int)m65ftp_files[fd].dir_sector_offset;
        m65ftp_files[fd].dir_sector_buf[off + 0x1C] = (m65ftp_files[fd].file_size >> 0) & 0xff;
        m65ftp_files[fd].dir_sector_buf[off + 0x1D] = (m65ftp_files[fd].file_size >> 8) & 0xff;
        m65ftp_files[fd].dir_sector_buf[off + 0x1E] = (m65ftp_files[fd].file_size >> 16) & 0xff;
        m65ftp_files[fd].dir_sector_buf[off + 0x1F] = (m65ftp_files[fd].file_size >> 24) & 0xff;
        write_sector(partition_start + m65ftp_files[fd].dir_sector, m65ftp_files[fd].dir_sector_buf);
    }

    pthread_mutex_unlock(&g_m65ftp_mutex);
    return (ssize_t)total_written;
}

ssize_t m65ftp_pread(int fd, unsigned char *buf, size_t count, off_t offset)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized || fd < 0 || fd >= m65ftp_file_count) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    if (m65ftp_files[fd].buffered) {
        unsigned int file_size = m65ftp_files[fd].file_size;
        if (count == 0) { pthread_mutex_unlock(&g_m65ftp_mutex); return 0; }
        if ((off_t)offset >= (off_t)file_size) { pthread_mutex_unlock(&g_m65ftp_mutex); return 0; }
        if ((off_t)(offset + count) > (off_t)file_size)
            count = file_size - offset;
        size_t avail = m65ftp_files[fd].write_buf_cap;
        if ((size_t)offset < avail) {
            size_t from_buf = count;
            if ((size_t)offset + from_buf > avail)
                from_buf = avail - (size_t)offset;
            memcpy(buf, m65ftp_files[fd].write_buf + offset, from_buf);
            if (from_buf < count)
                memset(buf + from_buf, 0, count - from_buf);
        } else {
            memset(buf, 0, count);
        }
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return (ssize_t)count;
    }

    unsigned int first_file_cluster = m65ftp_files[fd].first_cluster;
    unsigned int file_size = m65ftp_files[fd].file_size;

    if (count == 0 || first_file_cluster == 0) { pthread_mutex_unlock(&g_m65ftp_mutex); return 0; }
    if ((off_t)offset >= (off_t)file_size) { pthread_mutex_unlock(&g_m65ftp_mutex); return 0; }
    if ((off_t)(offset + count) > (off_t)file_size)
        count = file_size - offset;

    unsigned int cluster_size = sectors_per_cluster * 512;
    unsigned int first_data_sector = partition_start + reserved_sectors + 2 * sectors_per_fat;

    unsigned int cur_cluster = first_file_cluster;
    unsigned int cur_cluster_idx = 0;

    if (offset == m65ftp_files[fd].last_offset) {
        cur_cluster = m65ftp_files[fd].current_cluster;
        cur_cluster_idx = m65ftp_files[fd].current_cluster_idx;
    }

    size_t total_read = 0;
    size_t remaining = count;
    off_t current_offset = offset;

    while (remaining > 0 && cur_cluster < 0x0ffffff8) {
        unsigned int cluster_index = (unsigned int)(current_offset / cluster_size);
        unsigned int offset_in_cluster = (unsigned int)(current_offset % cluster_size);

        while (cur_cluster_idx < cluster_index) {
            cur_cluster = get_next_cluster(cur_cluster);
            cur_cluster_idx++;
            if (cur_cluster >= 0x0ffffff8)
                break;
        }
        if (cur_cluster >= 0x0ffffff8)
            break;

        unsigned int sector_in_cluster = offset_in_cluster / 512;
        unsigned int offset_in_sector = offset_in_cluster % 512;
        unsigned int sector_num = first_data_sector + (cur_cluster - 2) * sectors_per_cluster + sector_in_cluster;

        size_t bytes_from_sector = 512 - offset_in_sector;
        if (bytes_from_sector > remaining)
            bytes_from_sector = remaining;

        unsigned char sector_buf[512];
        if (read_sector(sector_num, sector_buf, CACHE_YES, 0)) {
            pthread_mutex_unlock(&g_m65ftp_mutex);
            m65ftp_files[fd].current_cluster = cur_cluster;
            m65ftp_files[fd].current_cluster_idx = cur_cluster_idx;
            m65ftp_files[fd].last_offset = current_offset;
            return total_read > 0 ? (ssize_t)total_read : -1;
        }

        memcpy(buf + total_read, sector_buf + offset_in_sector, bytes_from_sector);
        total_read += bytes_from_sector;
        current_offset += bytes_from_sector;
        remaining -= bytes_from_sector;
    }

    m65ftp_files[fd].current_cluster = cur_cluster;
    m65ftp_files[fd].current_cluster_idx = cur_cluster_idx;
    m65ftp_files[fd].last_offset = current_offset;
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return (ssize_t)total_read;
}

int m65ftp_stat(const char *path, unsigned int *file_size, int *is_dir, time_t *mtime)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    if (path[0] == '/' && path[1] == 0) {
        if (file_size) *file_size = 0;
        if (is_dir) *is_dir = 1;
        if (mtime) *mtime = time(NULL);
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return 0;
    }

    char dir_path[1024];
    char file_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (fat_opendir(dir_path, 0)) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    struct m65dirent de;
    while (fat_readdir(&de, 0) == 0) {
        if (de.d_name[0] == 0)
            continue;
        char *name = de.d_longname[0] ? de.d_longname : de.d_name;
        if (!strcmp(name, file_name)) {
            if (file_size) *file_size = de.d_filelen;
            if (is_dir) *is_dir = (de.d_attr & 0x10) ? 1 : 0;
            if (mtime) *mtime = mktime(&de.d_mtime);
            pthread_mutex_unlock(&g_m65ftp_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return -1;
}

int m65ftp_mkdir(const char *path)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    if (!path || !path[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char dir_path[1024];
    char dir_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(dir_name, last_slash + 1, sizeof(dir_name) - 1);
    dir_name[sizeof(dir_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!dir_name[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char saved_dir[1024];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    int ret = create_dir(dir_name);

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return ret ? -1 : 0;
}

int m65ftp_unlink(const char *path)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    if (!path || !path[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char dir_path[1024];
    char file_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!file_name[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char saved_dir[1024];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    /* Check if it's a directory — refuse */
    if (fat_opendir(dir_path, 0)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }
    struct m65dirent de;
    if (!find_file_in_curdir(file_name, &de)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }
    if (de.d_attr & 0x10) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    int ret = delete_single_file(file_name, TRUE);

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return ret ? -1 : 0;
}

int m65ftp_rmdir(const char *path)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    if (!path || !path[0] || strcmp(path, "/") == 0) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char dir_path[1024];
    char dir_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(dir_name, last_slash + 1, sizeof(dir_name) - 1);
    dir_name[sizeof(dir_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!dir_name[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char saved_dir[1024];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    /* Verify target is a directory */
    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    if (fat_opendir(dir_path, 0)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }
    struct m65dirent de;
    if (!find_file_in_curdir(dir_name, &de)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }
    if (!(de.d_attr & 0x10)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    /* Check if directory is empty */
    char full_path[1024];
    if (strcmp(dir_path, "/") == 0)
        snprintf(full_path, sizeof(full_path), "/%s", dir_name);
    else
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dir_name);

    strncpy(current_dir, full_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    if (fat_opendir(full_path, 0)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    struct m65dirent entry;
    while (fat_readdir(&entry, 0) == 0) {
        if (entry.d_name[0] == 0) continue;
        char *name = entry.d_longname[0] ? entry.d_longname : entry.d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        /* Directory is not empty */
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        current_dir[sizeof(current_dir) - 1] = 0;
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    /* Switch back to parent dir and delete */
    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    int ret = delete_single_file(dir_name, TRUE);

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return ret ? -1 : 0;
}

int m65ftp_open_writable(const char *path)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    if (m65ftp_file_count >= M65FTP_MAX_FILES) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char dir_path[1024];
    char file_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (fat_opendir(dir_path, 1)) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    struct m65dirent de;
    while (fat_readdir(&de, 0) == 0) {
        if (de.d_name[0] == 0)
            continue;
        char *name = de.d_longname[0] ? de.d_longname : de.d_name;
        if (!strcmp(name, file_name)) {
            int idx = m65ftp_file_count++;
            m65ftp_files[idx].first_cluster = de.d_ino;
            m65ftp_files[idx].file_size = de.d_filelen;
            m65ftp_files[idx].current_cluster = de.d_ino;
            m65ftp_files[idx].current_cluster_idx = 0;
            m65ftp_files[idx].last_offset = 0;
            m65ftp_files[idx].is_writable = 1;
            m65ftp_files[idx].buffered = 0;
            m65ftp_files[idx].write_buf = NULL;
            m65ftp_files[idx].write_buf_cap = 0;
            m65ftp_files[idx].dir_sector = dir_sector;
            m65ftp_files[idx].dir_sector_offset = dir_sector_offset;
            memcpy(m65ftp_files[idx].dir_sector_buf, dir_sector_buffer, 512);
            m65ftp_files[idx].write_cluster = de.d_ino;
            m65ftp_files[idx].sector_in_cluster = 0;
            pthread_mutex_unlock(&g_m65ftp_mutex);
            return idx;
        }
    }
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return -1;
}

static int count_chain(unsigned int first, unsigned int *last_out)
{
    unsigned int count = 1;
    unsigned int c = first;
    while (1) {
        unsigned int next = chained_cluster(c);
        if (next == 0 || next >= FAT32_MIN_END_OF_CLUSTER_MARKER)
            break;
        c = next;
        count++;
    }
    *last_out = c;
    return count;
}

int m65ftp_ftruncate(int fd, off_t length)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized || fd < 0 || fd >= m65ftp_file_count || !m65ftp_files[fd].is_writable) {
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    if (length < 0) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    if (m65ftp_files[fd].buffered) {
        if (length == 0) {
            free(m65ftp_files[fd].write_buf);
            m65ftp_files[fd].write_buf = NULL;
            m65ftp_files[fd].write_buf_cap = 0;
            m65ftp_files[fd].first_cluster = 0;
            m65ftp_files[fd].file_size = 0;
            m65ftp_files[fd].write_cluster = 0;
            m65ftp_files[fd].sector_in_cluster = 0;
            m65ftp_files[fd].last_offset = 0;
            m65ftp_files[fd].current_cluster = 0;
            m65ftp_files[fd].current_cluster_idx = 0;
            if (m65ftp_files[fd].dir_sector >= 0 && m65ftp_files[fd].dir_sector_offset >= 0) {
                unsigned int off = (unsigned int)m65ftp_files[fd].dir_sector_offset;
                m65ftp_files[fd].dir_sector_buf[off + 0x1C] = 0;
                m65ftp_files[fd].dir_sector_buf[off + 0x1D] = 0;
                m65ftp_files[fd].dir_sector_buf[off + 0x1E] = 0;
                m65ftp_files[fd].dir_sector_buf[off + 0x1F] = 0;
                m65ftp_files[fd].dir_sector_buf[off + 0x1A] = 0;
                m65ftp_files[fd].dir_sector_buf[off + 0x1B] = 0;
                m65ftp_files[fd].dir_sector_buf[off + 0x14] = 0;
                m65ftp_files[fd].dir_sector_buf[off + 0x15] = 0;
                write_sector(partition_start + m65ftp_files[fd].dir_sector, m65ftp_files[fd].dir_sector_buf);
                execute_write_queue();
            }
            pthread_mutex_unlock(&g_m65ftp_mutex);
            return 0;
        }
        m65ftp_files[fd].file_size = (unsigned int)length;
        if (m65ftp_files[fd].write_buf_cap < (size_t)length) {
            size_t new_cap = m65ftp_files[fd].write_buf_cap;
            if (new_cap == 0) new_cap = 4096;
            while (new_cap < (size_t)length) new_cap *= 2;
            if (new_cap > M65FTP_BUFFER_LIMIT) new_cap = M65FTP_BUFFER_LIMIT;
            unsigned char *nb = realloc(m65ftp_files[fd].write_buf, new_cap);
            if (!nb) {
                pthread_mutex_unlock(&g_m65ftp_mutex);
                return -1;
            }
            memset(nb + m65ftp_files[fd].write_buf_cap, 0, new_cap - m65ftp_files[fd].write_buf_cap);
            m65ftp_files[fd].write_buf = nb;
            m65ftp_files[fd].write_buf_cap = new_cap;
        }
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return 0;
    }

    unsigned int cluster_size = sectors_per_cluster * 512;
    unsigned int needed_clusters = length == 0
        ? 0
        : (unsigned int)(((unsigned long long)length + cluster_size - 1) / cluster_size);

    /* Truncate to zero → free everything (fast path, also handles zero-length file with no clusters) */
    if (needed_clusters == 0) {
        unsigned int c = m65ftp_files[fd].first_cluster;
        while (c > 0 && c < FAT32_MIN_END_OF_CLUSTER_MARKER) {
            unsigned int next = chained_cluster(c);
            deallocate_cluster(c);
            c = next;
        }

        if (m65ftp_files[fd].dir_sector >= 0 && m65ftp_files[fd].dir_sector_offset >= 0) {
            unsigned int off = (unsigned int)m65ftp_files[fd].dir_sector_offset;
            m65ftp_files[fd].dir_sector_buf[off + 0x1C] = 0;
            m65ftp_files[fd].dir_sector_buf[off + 0x1D] = 0;
            m65ftp_files[fd].dir_sector_buf[off + 0x1E] = 0;
            m65ftp_files[fd].dir_sector_buf[off + 0x1F] = 0;
            m65ftp_files[fd].dir_sector_buf[off + 0x1A] = 0;
            m65ftp_files[fd].dir_sector_buf[off + 0x1B] = 0;
            m65ftp_files[fd].dir_sector_buf[off + 0x14] = 0;
            m65ftp_files[fd].dir_sector_buf[off + 0x15] = 0;
            write_sector(partition_start + m65ftp_files[fd].dir_sector, m65ftp_files[fd].dir_sector_buf);
            execute_write_queue();
        }

        m65ftp_files[fd].first_cluster = 0;
        m65ftp_files[fd].file_size = 0;
        m65ftp_files[fd].write_cluster = 0;
        m65ftp_files[fd].sector_in_cluster = 0;
        m65ftp_files[fd].last_offset = 0;
        m65ftp_files[fd].current_cluster = 0;
        m65ftp_files[fd].current_cluster_idx = 0;

        pthread_mutex_unlock(&g_m65ftp_mutex);
        return 0;
    }

    /* Non-zero truncation */
    unsigned int first = m65ftp_files[fd].first_cluster;
    if (first == 0 || first >= FAT32_MIN_END_OF_CLUSTER_MARKER) {
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    unsigned int last;
    unsigned int current_count = count_chain(first, &last);

    if (needed_clusters > current_count) {
        /* Grow: always re-allocate the entire file as one contiguous block */
        unsigned int start = find_contiguous_clusters(needed_clusters);
        if (!start || start >= FAT32_MIN_END_OF_CLUSTER_MARKER) {
            pthread_mutex_unlock(&g_m65ftp_mutex);
            return -1;
        }

        /* Copy existing data from old clusters to new clusters before freeing old chain */
        unsigned int copy_size = m65ftp_files[fd].file_size;
        if (copy_size > (unsigned int)length)
            copy_size = (unsigned int)length;
        if (copy_size > 0) {
            unsigned int first_data_sector = partition_start + reserved_sectors + 2 * sectors_per_fat;
            unsigned char buf[512];
            unsigned int old_c = first;
            unsigned int new_c = start;
            unsigned int remaining_bytes = copy_size;
            while (remaining_bytes > 0) {
                unsigned int sector_in_c = 0;
                while (sector_in_c < sectors_per_cluster && remaining_bytes > 0) {
                    unsigned int old_sector = first_data_sector + (old_c - 2) * sectors_per_cluster + sector_in_c;
                    unsigned int new_sector = first_data_sector + (new_c - 2) * sectors_per_cluster + sector_in_c;
                    if (read_sector(old_sector, buf, CACHE_YES, 0)) {
                        pthread_mutex_unlock(&g_m65ftp_mutex);
                        return -1;
                    }
                    if (write_sector(new_sector, buf)) {
                        pthread_mutex_unlock(&g_m65ftp_mutex);
                        return -1;
                    }
                    remaining_bytes -= (remaining_bytes >= 512) ? 512 : remaining_bytes;
                    sector_in_c++;
                }
                if (remaining_bytes > 0) {
                    unsigned int next_old = chained_cluster(old_c);
                    if (next_old == 0 || next_old >= FAT32_MIN_END_OF_CLUSTER_MARKER)
                        break;
                    old_c = next_old;
                    new_c++;
                }
            }
        }

        /* Free the old chain entirely */
        {
            unsigned int c = first;
            while (c > 0 && c < FAT32_MIN_END_OF_CLUSTER_MARKER) {
                unsigned int next = chained_cluster(c);
                deallocate_cluster(c);
                c = next;
            }
        }

        /* Write new first_cluster into per-fd dir entry cache */
        if (m65ftp_files[fd].dir_sector >= 0 && m65ftp_files[fd].dir_sector_offset >= 0) {
            unsigned int off = (unsigned int)m65ftp_files[fd].dir_sector_offset;
            m65ftp_files[fd].dir_sector_buf[off + 0x1A] = start & 0xff;
            m65ftp_files[fd].dir_sector_buf[off + 0x1B] = (start >> 8) & 0xff;
            m65ftp_files[fd].dir_sector_buf[off + 0x14] = (start >> 16) & 0xff;
            m65ftp_files[fd].dir_sector_buf[off + 0x15] = (start >> 24) & 0xff;
        }

        /* Chain the contiguous block: allocate + chain each pair */
        {
            unsigned int prev = start;
            for (unsigned int i = 1; i < needed_clusters; i++) {
                unsigned int c = start + i;
                if (allocate_cluster(c)) {
                    pthread_mutex_unlock(&g_m65ftp_mutex);
                    return -1;
                }
                if (chain_cluster(prev, c)) {
                    pthread_mutex_unlock(&g_m65ftp_mutex);
                    return -1;
                }
                prev = c;
            }
            last = prev;
        }
        first = start;
    } else if (needed_clusters < current_count) {
        /* Shrink: walk to new last cluster, free everything after it */
        unsigned int c = first;
        for (unsigned int i = 1; i < needed_clusters; i++) {
            unsigned int next = chained_cluster(c);
            if (next == 0 || next >= FAT32_MIN_END_OF_CLUSTER_MARKER)
                break;
            c = next;
        }
        unsigned int free_start = chained_cluster(c);
        while (free_start > 0 && free_start < FAT32_MIN_END_OF_CLUSTER_MARKER) {
            unsigned int next = chained_cluster(free_start);
            deallocate_cluster(free_start);
            free_start = next;
        }
        set_fat_cluster_ptr(c, 0x0ffffff8);
        last = c;
    }

    /* Update file size and possibly first_cluster in per-fd dir entry cache, then write sector */
    if (m65ftp_files[fd].dir_sector >= 0 && m65ftp_files[fd].dir_sector_offset >= 0) {
        unsigned int off = (unsigned int)m65ftp_files[fd].dir_sector_offset;
        m65ftp_files[fd].dir_sector_buf[off + 0x1C] = (unsigned int)length & 0xff;
        m65ftp_files[fd].dir_sector_buf[off + 0x1D] = ((unsigned int)length >> 8) & 0xff;
        m65ftp_files[fd].dir_sector_buf[off + 0x1E] = ((unsigned int)length >> 16) & 0xff;
        m65ftp_files[fd].dir_sector_buf[off + 0x1F] = ((unsigned int)length >> 24) & 0xff;
        write_sector(partition_start + m65ftp_files[fd].dir_sector, m65ftp_files[fd].dir_sector_buf);
        execute_write_queue();
    }
    m65ftp_files[fd].file_size = (unsigned int)length;

    /* Persist first_cluster change if we re-allocated */
    m65ftp_files[fd].first_cluster = first;

    /* Reset write tracking to start of file */
    m65ftp_files[fd].write_cluster = first;
    m65ftp_files[fd].sector_in_cluster = 0;
    m65ftp_files[fd].last_offset = 0;
    m65ftp_files[fd].current_cluster = first;
    m65ftp_files[fd].current_cluster_idx = 0;

    pthread_mutex_unlock(&g_m65ftp_mutex);
    return 0;
}

int m65ftp_rename(const char *oldpath, const char *newpath)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    if (!oldpath || !oldpath[0] || !newpath || !newpath[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char old_dir[1024], old_name[256];
    strncpy(old_dir, oldpath, sizeof(old_dir) - 1);
    old_dir[sizeof(old_dir) - 1] = 0;
    char *last = strrchr(old_dir, '/');
    if (!last) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    strncpy(old_name, last + 1, sizeof(old_name) - 1);
    old_name[sizeof(old_name) - 1] = 0;
    if (last == old_dir) *(last + 1) = 0; else *last = 0;

    char new_dir[1024], new_name[256];
    strncpy(new_dir, newpath, sizeof(new_dir) - 1);
    new_dir[sizeof(new_dir) - 1] = 0;
    last = strrchr(new_dir, '/');
    if (!last) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    strncpy(new_name, last + 1, sizeof(new_name) - 1);
    new_name[sizeof(new_name) - 1] = 0;
    if (last == new_dir) *(last + 1) = 0; else *last = 0;

    if (!old_name[0] || !new_name[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    if (strcmp(old_dir, new_dir) != 0) {
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    char saved_dir[1024];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, old_dir, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    int ret = rename_file_or_dir(old_name, new_name);

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return ret ? -1 : 0;
}

int m65ftp_utimens(const char *path, const struct timespec tv[2])
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }
    if (!path || !path[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    if (strcmp(path, "/") == 0) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char dir_path[1024];
    char file_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (!file_name[0]) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

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

    char saved_dir[1024];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;

    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    struct m65dirent de;
    if (!find_file_in_curdir(file_name, &de)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    if (set_mtime) {
        struct tm *tm = localtime(&mtime);
        if (tm) {
            uint16_t time_word = (tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec >> 1);
            uint16_t date_word = ((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday;
            dir_sector_buffer[dir_sector_offset + 0x16] = time_word & 0xFF;
            dir_sector_buffer[dir_sector_offset + 0x17] = (time_word >> 8) & 0xFF;
            dir_sector_buffer[dir_sector_offset + 0x18] = date_word & 0xFF;
            dir_sector_buffer[dir_sector_offset + 0x19] = (date_word >> 8) & 0xFF;
        }
    }

    if (set_atime) {
        struct tm *tm = localtime(&atime);
        if (tm) {
            uint16_t date_word = ((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday;
            dir_sector_buffer[dir_sector_offset + 0x12] = date_word & 0xFF;
            dir_sector_buffer[dir_sector_offset + 0x13] = (date_word >> 8) & 0xFF;
        }
    }

    int ret = write_sector(partition_start + dir_sector, dir_sector_buffer);
    if (!ret)
        execute_write_queue();

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return ret ? -1 : 0;
}

int m65ftp_is_fragmented(const char *path)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char dir_path[1024];
    char file_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    if (fat_opendir(dir_path, 0)) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    char saved_dir[1024];
    strncpy(saved_dir, current_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = 0;
    strncpy(current_dir, dir_path, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = 0;

    struct m65dirent de;
    if (!find_file_in_curdir(file_name, &de)) {
        strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    strncpy(current_dir, saved_dir, sizeof(current_dir) - 1);

    unsigned int current_cluster = calc_first_cluster_of_file();
    unsigned int next_cluster;
    while ((next_cluster = chained_cluster(current_cluster)) < FAT32_MIN_END_OF_CLUSTER_MARKER) {
        if (next_cluster != current_cluster + 1) {
            pthread_mutex_unlock(&g_m65ftp_mutex);
            return 1;
        }
        current_cluster = next_cluster;
    }

    pthread_mutex_unlock(&g_m65ftp_mutex);
    return 0;
}

int m65ftp_readdir(const char *path,
    int (*cb)(const char *name, unsigned int file_size, int is_dir, time_t mtime, void *ctx),
    void *ctx)
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    if (fat_opendir((char *)path, 0)) { pthread_mutex_unlock(&g_m65ftp_mutex); return -1; }

    struct m65dirent de;
    while (fat_readdir(&de, 0) == 0) {
        if (de.d_name[0] == 0)
            continue;
        char *name = de.d_longname[0] ? de.d_longname : de.d_name;
        int is_dir = (de.d_attr & 0x10) ? 1 : 0;
        time_t mtime = mktime(&de.d_mtime);
        if (cb(name, de.d_filelen, is_dir, mtime, ctx))
            break;
    }
    pthread_mutex_unlock(&g_m65ftp_mutex);
    return 0;
}

int m65ftp_mbrinfo(struct m65ftp_partition partitions[4])
{
    pthread_mutex_lock(&g_m65ftp_mutex);
    if (!m65ftp_initialized) {
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    unsigned char mbr[512];
    // MBR is always at absolute sector 0 on the SD card
    if (read_sector(0, mbr, CACHE_NO, 0) != 0) {
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    // Check MBR signature 0x55AA at offset 510-511
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        pthread_mutex_unlock(&g_m65ftp_mutex);
        return -1;
    }

    int count = 0;
    // MBR partition table starts at offset 0x1BE (4 entries of 16 bytes)
    for (int i = 0; i < 4; i++) {
        int base = 0x1BE + (i * 16);
        unsigned char type = mbr[base + 4];
        
        // Little-endian 32-bit integers for start LBA and sector count
        unsigned int start_lba = mbr[base + 8] | 
                                 (mbr[base + 9] << 8) | 
                                 (mbr[base + 10] << 16) | 
                                 (mbr[base + 11] << 24);
                                 
        unsigned int sectors = mbr[base + 12] | 
                               (mbr[base + 13] << 8) | 
                               (mbr[base + 14] << 16) | 
                               (mbr[base + 15] << 24);

        partitions[i].type = type;
        partitions[i].start_sector = start_lba;
        partitions[i].sector_count = sectors;

        if (type != 0 && sectors > 0) {
            count++;
        }
    }

    pthread_mutex_unlock(&g_m65ftp_mutex);
    return count;
}
