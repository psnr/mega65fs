#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>

#include "../libm65ftp/m65ftp_lib.h"

#define ANSI_RED    "\033[31m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_RESET  "\033[0m"

#define MAX_ENTRIES 512
#define MAX_PATH 512
#define MAX_CACHED_DIRS 16
#define CACHE_TTL_SECONDS 5

typedef struct {
    char name[256];
    size_t size;
    bool is_dir;
    time_t mtime;
    bool valid;
} directory_entry_t;

typedef struct {
    char path[MAX_PATH];
    directory_entry_t entries[MAX_ENTRIES];
    int entry_count;
    time_t last_updated;
    bool valid;
} dir_cache_t;

// Mutexes (lock ordering: g_cache_mutex -> g_ftp access)
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Directory cache slots
static dir_cache_t g_cache_slots[MAX_CACHED_DIRS] = {0};

/* ========================================================================= *
 * DIRECTORY CACHE                                                           *
 * ========================================================================= */

static dir_cache_t* get_cache_slot(const char *path) {
    int oldest_idx = 0;
    time_t oldest_time = time(NULL) + 1;

    for (int i = 0; i < MAX_CACHED_DIRS; i++) {
        if (g_cache_slots[i].valid && strcmp(g_cache_slots[i].path, path) == 0)
            return &g_cache_slots[i];
        if (!g_cache_slots[i].valid)
            return &g_cache_slots[i];
        if (g_cache_slots[i].last_updated < oldest_time) {
            oldest_time = g_cache_slots[i].last_updated;
            oldest_idx = i;
        }
    }
    return &g_cache_slots[oldest_idx];
}

struct fill_ctx {
    dir_cache_t *cache;
};

static int fill_dir_cache(const char *name, unsigned int file_size, int is_dir, time_t mtime, void *ctx) {
    struct fill_ctx *fc = (struct fill_ctx *)ctx;
    if (fc->cache->entry_count >= MAX_ENTRIES)
        return 1;
    directory_entry_t *e = &fc->cache->entries[fc->cache->entry_count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
    e->size = file_size;
    e->is_dir = is_dir != 0;
    e->mtime = mtime;
    e->valid = true;
    return 0;
}

static dir_cache_t* refresh_cache_if_expired(const char *path) {
    pthread_mutex_lock(&g_cache_mutex);

    time_t now = time(NULL);
    dir_cache_t *slot = get_cache_slot(path);

    if (slot->valid && strcmp(slot->path, path) == 0 && (now - slot->last_updated) < CACHE_TTL_SECONDS) {
        pthread_mutex_unlock(&g_cache_mutex);
        return slot;
    }

    slot = get_cache_slot(path);
    memset(slot, 0, sizeof(dir_cache_t));
    strncpy(slot->path, path, sizeof(slot->path) - 1);

    struct fill_ctx fc = { .cache = slot };
    m65ftp_readdir(path, fill_dir_cache, &fc);

    slot->last_updated = now;
    slot->valid = true;

    pthread_mutex_unlock(&g_cache_mutex);
    return slot;
}

/* ========================================================================= *
 * CACHE-AWARE STAT                                                          *
 * ========================================================================= */

static int stat_from_cache(const char *path, unsigned int *file_size, int *is_dir, time_t *mtime, time_t now)
{
    char dir_path[MAX_PATH];
    char file_name[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash) return -1;

    strncpy(file_name, last_slash + 1, sizeof(file_name) - 1);
    file_name[sizeof(file_name) - 1] = 0;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    for (int i = 0; i < MAX_CACHED_DIRS; i++) {
        if (!g_cache_slots[i].valid || strcmp(g_cache_slots[i].path, dir_path) != 0)
            continue;
        if (now - g_cache_slots[i].last_updated >= CACHE_TTL_SECONDS)
            return -1;
        for (int j = 0; j < g_cache_slots[i].entry_count; j++) {
            if (!g_cache_slots[i].entries[j].valid)
                continue;
            if (strcmp(g_cache_slots[i].entries[j].name, file_name) == 0) {
                if (file_size) *file_size = (unsigned int)g_cache_slots[i].entries[j].size;
                if (is_dir) *is_dir = g_cache_slots[i].entries[j].is_dir ? 1 : 0;
                if (mtime) *mtime = g_cache_slots[i].entries[j].mtime;
                return 0;
            }
        }
        return -1;
    }
    return -1;
}

/* ========================================================================= *
 * FUSE CALLBACK IMPLEMENTATIONS                                             *
 * ========================================================================= */

static void invalidate_cache_parent(const char *path) {
    char parent[MAX_PATH];
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = 0;
    char *slash = strrchr(parent, '/');
    if (!slash) return;
    if (slash == parent)
        *(slash + 1) = 0;
    else
        *slash = 0;
    pthread_mutex_lock(&g_cache_mutex);
    for (int i = 0; i < MAX_CACHED_DIRS; i++) {
        if (g_cache_slots[i].valid && strcmp(g_cache_slots[i].path, parent) == 0) {
            g_cache_slots[i].valid = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_cache_mutex);
}

static void* fs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void) conn;
    (void) cfg;
    printf("FUSE initialized. Connecting to MEGA65 via ethernet...\n");
    if (m65ftp_init() != 0) {
        fprintf(stderr, "[ERROR] Failed to connect to MEGA65 via ethernet.\n");
    } else {
        printf("Connected to MEGA65 successfully.\n");
    }
    return NULL;
}

static void fs_destroy(void *private_data) {
    (void) private_data;
    printf("Unmounting filesystem. Disconnecting from MEGA65...\n");
    m65ftp_finish();
}

static int fs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;

    if (strlen(path) >= MAX_PATH) return -ENAMETOOLONG;

    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    unsigned int file_size = 0;
    int is_dir = 0;
    time_t mtime = 0;
    time_t now = time(NULL);

    pthread_mutex_lock(&g_cache_mutex);
    int found = stat_from_cache(path, &file_size, &is_dir, &mtime, now);
    pthread_mutex_unlock(&g_cache_mutex);

    if (found != 0) {
        if (m65ftp_stat(path, &file_size, &is_dir, &mtime) != 0)
            return -ENOENT;
    }

    if (is_dir) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_size = 4096;
    } else {
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = file_size;
    }
    stbuf->st_atim.tv_sec = mtime;
    stbuf->st_mtim.tv_sec = mtime;
    stbuf->st_ctim.tv_sec = mtime;
    return 0;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void) fi;
    (void) flags;

    if (strlen(path) >= MAX_PATH) return -ENAMETOOLONG;

    struct stat st_dot, st_ddot;
    memset(&st_dot, 0, sizeof(st_dot));
    memset(&st_ddot, 0, sizeof(st_ddot));

    st_dot.st_mode = S_IFDIR | 0755;
    st_dot.st_nlink = 2;
    st_ddot.st_mode = S_IFDIR | 0755;
    st_ddot.st_nlink = 2;

    off_t pos = 0;

    if (pos >= offset)
        filler(buf, ".", &st_dot, 1, 0);
    pos++;

    if (pos >= offset)
        filler(buf, "..", &st_ddot, 2, 0);
    pos++;

    dir_cache_t *cache = refresh_cache_if_expired(path);

    for (int i = 0; i < cache->entry_count; i++) {
        if (!cache->entries[i].valid)
            continue;
        if (pos >= offset) {
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_mode = cache->entries[i].is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
            st.st_nlink = cache->entries[i].is_dir ? 2 : 1;
            st.st_size = cache->entries[i].size;
            st.st_mtim.tv_sec = cache->entries[i].mtime;

            if (filler(buf, cache->entries[i].name, &st, pos + 1, 0) != 0) break;
        }
        pos++;
    }

    return 0;
}

static int fs_open(const char *path, struct fuse_file_info *fi) {
    if (strlen(path) >= MAX_PATH) return -ENAMETOOLONG;

    int acc = fi->flags & O_ACCMODE;
    int writable = (acc == O_WRONLY || acc == O_RDWR);

    int fd;
    if (writable)
        fd = m65ftp_open_writable(path);
    else
        fd = m65ftp_open(path);

    if (fd < 0)
        return -ENOENT;

    fi->fh = (uint64_t)(intptr_t)(size_t)fd;
    return 0;
}

static int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)mode;
    if (strlen(path) >= MAX_PATH) return -ENAMETOOLONG;

    int fd = m65ftp_create(path);
    if (fd < 0)
        return -EIO;

    fi->fh = (uint64_t)(intptr_t)(size_t)fd;
    invalidate_cache_parent(path);
    return 0;
}

static int fs_write(const char *path, const char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void)path;
    int fd = (int)(intptr_t)(size_t)fi->fh;
    ssize_t res = m65ftp_pwrite(fd, (const unsigned char *)buf, size, offset);
    if (res < 0)
        return -EIO;
    return (int)res;
}

static int fs_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
    (void) path;

    int fd = (int)(intptr_t)(size_t)fi->fh;
    ssize_t res = m65ftp_pread(fd, (unsigned char *)buf, size, offset);
    if (res < 0)
        return -EIO;

    return (int)res;
}

static int fs_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    int fd = (int)(intptr_t)(size_t)fi->fh;
    m65ftp_close(fd);
    return 0;
}

static int fs_truncate(const char *path, off_t length, struct fuse_file_info *fi) {
    (void)path;
    int fd = (int)(intptr_t)(size_t)fi->fh;
    if (m65ftp_ftruncate(fd, length) != 0)
        return -EIO;
    return 0;
}

static int fs_utimens(const char *path, const struct timespec tv[2],
                       struct fuse_file_info *fi) {
    (void)fi;
    if (strlen(path) >= MAX_PATH) return -ENAMETOOLONG;

    if (m65ftp_utimens(path, tv) != 0)
        return -ENOENT;
    return 0;
}

static int fs_mkdir(const char *path, mode_t mode) {
    (void)mode;
    if (strlen(path) >= MAX_PATH) return -ENAMETOOLONG;
    if (m65ftp_mkdir(path) != 0)
        return -EIO;
    invalidate_cache_parent(path);
    return 0;
}

static int fs_rmdir(const char *path) {
    if (strlen(path) >= MAX_PATH) return -ENAMETOOLONG;
    int ret = m65ftp_rmdir(path);
    if (ret != 0)
        return -ENOTEMPTY;
    invalidate_cache_parent(path);
    return 0;
}

static int fs_unlink(const char *path) {
    if (strlen(path) >= MAX_PATH) return -ENAMETOOLONG;
    int ret = m65ftp_unlink(path);
    if (ret != 0)
        return -EISDIR;
    invalidate_cache_parent(path);
    return 0;
}

static struct fuse_operations fs_oper = {
    .init     = fs_init,
    .destroy  = fs_destroy,
    .getattr  = fs_getattr,
    .readdir  = fs_readdir,
    .open     = fs_open,
    .read     = fs_read,
    .write    = fs_write,
    .create   = fs_create,
    .release  = fs_release,
    .truncate = fs_truncate,
    .utimens  = fs_utimens,
    .mkdir    = fs_mkdir,
    .rmdir    = fs_rmdir,
    .unlink   = fs_unlink,
};

#define SCAN_MAX_DEPTH 32

struct scan_entry {
    char name[256];
    int is_dir;
};

struct scan_ctx {
    struct scan_entry *entries;
    int count;
    int cap;
};

static int scan_collect_cb(const char *name, unsigned int file_size, int is_dir, time_t mtime, void *ctx) {
    (void)file_size;
    (void)mtime;
    if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
        return 0;
    struct scan_ctx *s = (struct scan_ctx *)ctx;
    if (s->count >= s->cap) {
        int newcap = s->cap ? s->cap * 2 : 256;
        struct scan_entry *newp = realloc(s->entries, newcap * sizeof(struct scan_entry));
        if (!newp) return 1;
        s->entries = newp;
        s->cap = newcap;
    }
    strncpy(s->entries[s->count].name, name, sizeof(s->entries[0].name) - 1);
    s->entries[s->count].name[sizeof(s->entries[0].name) - 1] = 0;
    s->entries[s->count].is_dir = is_dir;
    s->count++;
    return 0;
}

static void scan_dir_recursive(const char *path, const char *prefix,
                               int *total_files, int *total_fragmented)
{
    struct scan_ctx s = {0};

    if (m65ftp_readdir(path, scan_collect_cb, &s))
        goto done;

    for (int i = 0; i < s.count; i++) {
        const char *stem = (i == s.count - 1) ? "\342\224\224\342\224\200 " : "\342\224\234\342\224\200 ";
        const char *cont = (i == s.count - 1) ? "   " : "\342\224\202  ";

        char full[1024];
        size_t plen = strlen(path);
        if (plen > 0 && path[plen - 1] == '/')
            snprintf(full, sizeof(full), "%s%s", path, s.entries[i].name);
        else
            snprintf(full, sizeof(full), "%s/%s", path, s.entries[i].name);

        if (s.entries[i].is_dir) {
            printf("%s%s%s\n", prefix, stem, s.entries[i].name);
            if (strlen(prefix) + strlen(cont) < 512) {
                char new_prefix[1024];
                snprintf(new_prefix, sizeof(new_prefix), "%s%s", prefix, cont);
                scan_dir_recursive(full, new_prefix, total_files, total_fragmented);
            }
        } else {
            int frag = m65ftp_is_fragmented(full);
            const char *color, *status;
            if (frag == 1)      { color = ANSI_RED;   status = "FRAGMENTED"; (*total_fragmented)++; }
            else if (frag == 0) { color = ANSI_GREEN; status = "OK"; }
            else                { color = ANSI_RED;   status = "ERROR"; }
            printf("%s%s%s: %s%s%s\n", prefix, stem, s.entries[i].name, color, status, ANSI_RESET);
            (*total_files)++;
        }
    }

done:
    free(s.entries);
}

static int defrag_file(const char *full_path, unsigned int file_size)
{
    if (file_size == 0) return 0;

    char dir_path[1024];
    strncpy(dir_path, full_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = 0;
    char *slash = strrchr(dir_path, '/');
    if (!slash) return -1;
    *slash = 0;
    const char *file_name = slash + 1;
    if (!file_name[0]) return -1;
    if (strlen(dir_path) == 0)
        strcpy(dir_path, "/");

    static unsigned int temp_id = 0;
    char temp_name[64];
    snprintf(temp_name, sizeof(temp_name), "._defrag_%04x", temp_id++);
    char temp_path[1024];
    if (strcmp(dir_path, "/") == 0)
        snprintf(temp_path, sizeof(temp_path), "/%s", temp_name);
    else
        snprintf(temp_path, sizeof(temp_path), "%s/%s", dir_path, temp_name);

    int tmp_fd = m65ftp_create(temp_path);
    if (tmp_fd < 0) { fprintf(stderr, "  [ERROR] create temp failed\n"); return -1; }

    /* For very large files, fall back to streaming copy */
    if (file_size > (256UL * 1024 * 1024)) {
        int is_contiguous = 0;
        if (m65ftp_ftruncate(tmp_fd, file_size) == 0)
            is_contiguous = 1;

        int orig_fd = m65ftp_open(full_path);
        if (orig_fd < 0) {
            fprintf(stderr, "  [ERROR] open original failed\n");
            m65ftp_close(tmp_fd);
            m65ftp_unlink(temp_path);
            return -1;
        }

        unsigned char buf[65536];
        off_t pos = 0;
        int last_pct = -1;
        while ((off_t)pos < (off_t)file_size) {
            size_t to_read = sizeof(buf);
            size_t remain = file_size - (unsigned int)pos;
            if (to_read > remain) to_read = remain;
            ssize_t n = m65ftp_pread(orig_fd, buf, to_read, pos);
            if (n <= 0) { fprintf(stderr, "  [ERROR] pread failed at offset %lld (return %zd)\n", (long long)pos, n); break; }
            ssize_t written = m65ftp_pwrite(tmp_fd, buf, (size_t)n, pos);
            if (written != n) { fprintf(stderr, "  [ERROR] pwrite failed at offset %lld (wrote %zd, expected %zu)\n", (long long)pos, written, n); pos = -1; break; }
            pos += written;
            int pct = (int)(100ULL * (unsigned long long)pos / file_size);
            if (pct != last_pct) {
                printf("\r\033[K%s: defragmenting... %3d%%", full_path, pct);
                fflush(stdout);
                last_pct = pct;
            }
        }
        m65ftp_close(orig_fd);
        m65ftp_close(tmp_fd);
        if (pos != (off_t)file_size) { m65ftp_unlink(temp_path); return -1; }
        if (m65ftp_unlink(full_path) != 0) { fprintf(stderr, "  [ERROR] unlink original after streaming copy failed\n"); m65ftp_unlink(temp_path); return -1; }
        if (m65ftp_rename(temp_path, full_path) != 0) { fprintf(stderr, "  [ERROR] rename failed\n"); return -1; }
        return is_contiguous ? 0 : 1;
    }

    /* Read all original data into host memory */
    unsigned char *data = malloc(file_size ? file_size : 1);
    if (!data) { fprintf(stderr, "  [ERROR] malloc(%u) failed\n", file_size); m65ftp_close(tmp_fd); m65ftp_unlink(temp_path); return -1; }

    int orig_fd = m65ftp_open(full_path);
    if (orig_fd < 0) { fprintf(stderr, "  [ERROR] open original for reading failed\n"); free(data); m65ftp_close(tmp_fd); m65ftp_unlink(temp_path); return -1; }

    off_t pos = 0;
    while ((off_t)pos < (off_t)file_size) {
        size_t to_read = 65536;
        size_t remain = file_size - (unsigned int)pos;
        if (to_read > remain) to_read = remain;
        ssize_t n = m65ftp_pread(orig_fd, data + pos, to_read, pos);
        if (n <= 0) { fprintf(stderr, "  [WARN] pread returned %zd at offset %lld (cluster chain shorter than file size)\n", n, (long long)pos); break; }
        pos += n;
    }
    m65ftp_close(orig_fd);

    unsigned int actual_size = (unsigned int)pos;
    if (actual_size != file_size) {
        fprintf(stderr, "  [INFO] truncated to %u bytes (declared %u)\n", actual_size, file_size);
        file_size = actual_size;
    }

    /* Free original clusters BEFORE allocating temp block */
    if (m65ftp_unlink(full_path) != 0) { fprintf(stderr, "  [ERROR] unlink original failed\n"); free(data); m65ftp_unlink(temp_path); return -1; }

    /* Try contiguous allocation — original clusters now available */
    int is_contiguous = 0;
    if (file_size > 0 && m65ftp_ftruncate(tmp_fd, file_size) == 0)
        is_contiguous = 1;

    int last_pct = -1;
    pos = 0;
    while ((off_t)pos < (off_t)file_size) {
        size_t to_write = 65536;
        size_t remain = file_size - (unsigned int)pos;
        if (to_write > remain) to_write = remain;
        ssize_t written = m65ftp_pwrite(tmp_fd, data + pos, to_write, pos);
        if (written != (ssize_t)to_write) { fprintf(stderr, "  [ERROR] pwrite temp at offset %lld returned %zd (expected %zu)\n", (long long)pos, written, to_write); pos = -1; break; }
        pos += written;
        int pct = (int)(100ULL * (unsigned long long)pos / file_size);
        if (pct != last_pct) {
            printf("\r\033[K%s: defragmenting... %3d%%", full_path, pct);
            fflush(stdout);
            last_pct = pct;
        }
    }

    free(data);
    m65ftp_close(tmp_fd);
    if (pos != (off_t)file_size) { m65ftp_unlink(temp_path); return -1; }

    if (m65ftp_rename(temp_path, full_path) != 0) { fprintf(stderr, "  [ERROR] rename %s -> %s failed\n", temp_path, full_path); return -1; }
    return is_contiguous ? 0 : 1;
}

static void defrag_dir_recursive(const char *path, const char *prefix,
                                 int *total_files, int *total_defragged,
                                 int *total_errors)
{
    struct scan_ctx s = {0};

    if (m65ftp_readdir(path, scan_collect_cb, &s))
        goto done;

    for (int i = 0; i < s.count; i++) {
        const char *stem = (i == s.count - 1) ? "\342\224\224\342\224\200 " : "\342\224\234\342\224\200 ";
        const char *cont = (i == s.count - 1) ? "   " : "\342\224\202  ";

        char full[1024];
        size_t plen = strlen(path);
        if (plen > 0 && path[plen - 1] == '/')
            snprintf(full, sizeof(full), "%s%s", path, s.entries[i].name);
        else
            snprintf(full, sizeof(full), "%s/%s", path, s.entries[i].name);

        if (s.entries[i].is_dir) {
            printf("%s%s%s\n", prefix, stem, s.entries[i].name);
            if (strlen(prefix) + strlen(cont) < 512) {
                char new_prefix[1024];
                snprintf(new_prefix, sizeof(new_prefix), "%s%s", prefix, cont);
                defrag_dir_recursive(full, new_prefix, total_files, total_defragged, total_errors);
            }
        } else {
            int frag = m65ftp_is_fragmented(full);
            if (frag == 1) {
                unsigned int file_size = 0;
                if (m65ftp_stat(full, &file_size, NULL, NULL) != 0) {
                    printf("%s%s%s: %sERROR (stat)%s\n", prefix, stem, s.entries[i].name, ANSI_RED, ANSI_RESET);
                    (*total_errors)++;
                    (*total_files)++;
                    continue;
                }
                int ok = defrag_file(full, file_size);
                if (ok == 0) {
                    printf("\r\033[K%s%s%s: %sdefragmented (%u bytes -> 1 fragment)%s\n", prefix, stem, s.entries[i].name, ANSI_GREEN, file_size, ANSI_RESET);
                    (*total_defragged)++;
                } else if (ok == 1) {
                    printf("\r\033[K%s%s%s: defragmented (fallback, fewer fragments)\n", prefix, stem, s.entries[i].name);
                    (*total_defragged)++;
                } else {
                    printf("\r\033[K%s%s%s: %sFAILED (insufficient contiguous space)%s\n", prefix, stem, s.entries[i].name, ANSI_RED, ANSI_RESET);
                    (*total_errors)++;
                }
            } else if (frag == 0) {
                printf("%s%s%s: %sOK%s\n", prefix, stem, s.entries[i].name, ANSI_GREEN, ANSI_RESET);
            } else {
                printf("%s%s%s: %sERROR%s\n", prefix, stem, s.entries[i].name, ANSI_RED, ANSI_RESET);
                (*total_errors)++;
            }
            (*total_files)++;
        }
    }

done:
    free(s.entries);
}

static int cmd_defrag(int argc, char *argv[])
{
    const char *root = "/";
    if (argc >= 3)
        root = argv[2];

    printf("Connecting to MEGA65 for defrag...\n");
    if (m65ftp_init() != 0) {
        fprintf(stderr, "Failed to connect.\n");
        return 1;
    }

    /* Scan the tree first to report status */
    printf("\n--- Scan: %s ---\n", root);
    int total_files = 0, total_fragmented = 0;
    scan_dir_recursive(root, "", &total_files, &total_fragmented);
    printf("Scan complete. %d files, %d fragmented.\n", total_files, total_fragmented);

    if (total_fragmented == 0) {
        printf("Nothing to defrag.\n");
        m65ftp_finish();
        return 0;
    }

    /* Defrag */
    printf("\n--- Defragmenting ---\n");
    int total_defragged = 0, total_errors = 0;
    total_files = 0;
    defrag_dir_recursive(root, "", &total_files, &total_defragged, &total_errors);

    printf("\nDefrag complete. %d defragmented, %d errors.\n", total_defragged, total_errors);
    m65ftp_finish();
    return total_errors ? 1 : 0;
}

static char g_mountpoint[1024] = {0};

static void cleanup_mount(void)
{
    if (g_mountpoint[0]) {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "fusermount -u %s 2>/dev/null", g_mountpoint);
        system(cmd);
    }
}

static void signal_handler(int sig)
{
    (void)sig;
    cleanup_mount();
    _exit(1);
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--defrag") == 0) {
        return cmd_defrag(argc, argv);
    }

    for (int i = 1; i < argc - 1; i++) {
        if (argv[i][0] != '-') {
            strncpy(g_mountpoint, argv[i], sizeof(g_mountpoint) - 1);
            g_mountpoint[sizeof(g_mountpoint) - 1] = 0;
            break;
        }
    }

    cleanup_mount();
    atexit(cleanup_mount);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    printf("Starting mega65fs filesystem driver...\n");
    return fuse_main(argc, argv, &fs_oper, NULL);
}
