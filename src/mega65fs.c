/* ========================================================================= *
 * mega65fs.c — MEGA65 SD card network filesystem (FUSE driver).
 *
 * Mounts the SD card of a MEGA65 over the network FTP connection provided
 * by m65ftp_lib as a local FUSE filesystem.  A short-lived directory cache
 * (see "Directory cache" below) keeps repeated stat/readdir traffic off
 * the network by reusing listing results for a few seconds at a time.
 * ========================================================================= */

/* mega65fs is Linux-only (FUSE 3 + fusermount). */
#if !defined(__linux__)
#error "mega65fs is Linux-only: requires FUSE 3 and fusermount. macOS and Windows are not supported."
#endif

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#include "m65ftp_lib.h"

/* ------------------------------------------------------------------- *
 * Constants
 * ------------------------------------------------------------------- */

#define MAX_ENTRIES       512
#define MAX_PATH          512
#define ENTRY_NAME_MAX    256
#define MAX_CACHED_DIRS   16
#define CACHE_TTL_SECONDS 5

#define DIR_MODE          0755
#define FILE_MODE         0644
#define DIR_NLINK         2
#define FILE_NLINK        1
#define DIR_SIZE          4096
#define DIR_BLOCKS        8
#define SECTOR_SIZE       512
#define STATFS_FRSIZE     4096
#define STATFS_FILES      65535
#define STATFS_FREE_FILES 60000
#define STATFS_NAME_MAX   255
#define FALLBACK_TOTAL_BYTES 13782993408ULL
#define MOUNTPOINT_SIZE   1024

/* ------------------------------------------------------------------- *
 * Types
 * ------------------------------------------------------------------- */

typedef struct {
    char name[ENTRY_NAME_MAX];
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

struct fill_ctx {
    dir_cache_t *cache;
};

/* Defined in "Small helpers" below; used here by stat_from_cache. */
static const char *split_parent(char *dir_path, size_t dir_size,
                                const char *path);

/* ------------------------------------------------------------------- *
 * Directory cache
 *
 * A small set of directory listings, keyed by path.  Entries are reused
 * while they are younger than CACHE_TTL_SECONDS, so stat/readdir bursts
 * stay local.  Lock ordering is always s_cache_mutex -> FTP access.
 * ------------------------------------------------------------------- */

static pthread_mutex_t s_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static dir_cache_t s_cache_slots[MAX_CACHED_DIRS] = {0};

/* Return the cache slot that should hold the listing for path: either a
 * matching valid slot, an empty slot, or the least recently updated one. */
static dir_cache_t *get_cache_slot(const char *path)
{
    int oldest_idx = 0;
    time_t oldest_time = time(NULL) + 1;

    for (int i = 0; i < MAX_CACHED_DIRS; i++) {
        if (s_cache_slots[i].valid && strcmp(s_cache_slots[i].path, path) == 0)
            return &s_cache_slots[i];
        if (!s_cache_slots[i].valid)
            return &s_cache_slots[i];
        if (s_cache_slots[i].last_updated < oldest_time) {
            oldest_time = s_cache_slots[i].last_updated;
            oldest_idx = i;
        }
    }
    return &s_cache_slots[oldest_idx];
}

/* m65ftp_readdir callback: append one entry to the cache, skipping "."
 * and ".." so they are not duplicated by readdir.  Returns non-zero once
 * the slot is full to stop the listing. */
static int fill_dir_cache(const char *name, unsigned int file_size, int is_dir,
                          time_t mtime, void *ctx)
{
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
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

/* Return the (still locked) cache slot for path, refreshing it from the
 * MEGA65 when it is missing or older than CACHE_TTL_SECONDS. */
static dir_cache_t *refresh_cache_if_expired(const char *path)
{
    pthread_mutex_lock(&s_cache_mutex);

    time_t now = time(NULL);
    dir_cache_t *slot = get_cache_slot(path);

    if (slot->valid && strcmp(slot->path, path) == 0 &&
        (now - slot->last_updated) < CACHE_TTL_SECONDS) {
        pthread_mutex_unlock(&s_cache_mutex);
        return slot;
    }

    slot = get_cache_slot(path);
    memset(slot, 0, sizeof(dir_cache_t));
    strncpy(slot->path, path, sizeof(slot->path) - 1);

    struct fill_ctx fc = { .cache = slot };
    m65ftp_readdir(path, fill_dir_cache, &fc);

    slot->last_updated = now;
    slot->valid = true;

    pthread_mutex_unlock(&s_cache_mutex);
    return slot;
}

/* Look up one entry in the cache.  Returns 0 when the entry was found and
 * filled in, -1 when the path is not in the cache (or the listing is too
 * old to trust). */
static int stat_from_cache(const char *path, unsigned int *file_size,
                           int *is_dir, time_t *mtime, time_t now)
{
    char dir_path[MAX_PATH];
    const char *file_name = split_parent(dir_path, sizeof(dir_path), path);
    if (!file_name)
        return -1;

    for (int i = 0; i < MAX_CACHED_DIRS; i++) {
        if (!s_cache_slots[i].valid ||
            strcmp(s_cache_slots[i].path, dir_path) != 0)
            continue;
        if (now - s_cache_slots[i].last_updated >= CACHE_TTL_SECONDS)
            return -1;
        for (int j = 0; j < s_cache_slots[i].entry_count; j++) {
            if (!s_cache_slots[i].entries[j].valid)
                continue;
            if (strcmp(s_cache_slots[i].entries[j].name, file_name) == 0) {
                if (file_size)
                    *file_size = (unsigned int)s_cache_slots[i].entries[j].size;
                if (is_dir)
                    *is_dir = s_cache_slots[i].entries[j].is_dir ? 1 : 0;
                if (mtime)
                    *mtime = s_cache_slots[i].entries[j].mtime;
                return 0;
            }
        }
        return -1;
    }
    return -1;
}

/* ------------------------------------------------------------------- *
 * Small helpers
 * ------------------------------------------------------------------- */

/* Copy path into dir_path and truncate it at its final '/', so dir_path
 * holds the parent directory.  Returns a pointer to the basename within
 * dir_path, or NULL when path contains no '/'. */
static const char *split_parent(char *dir_path, size_t dir_size,
                                const char *path)
{
    strncpy(dir_path, path, dir_size - 1);
    dir_path[dir_size - 1] = 0;

    char *last_slash = strrchr(dir_path, '/');
    if (!last_slash)
        return NULL;

    const char *name = last_slash + 1;

    if (last_slash == dir_path)
        *(last_slash + 1) = 0;
    else
        *last_slash = 0;

    return name;
}

/* Invalidate any cached listing for the parent directory of path, so the
 * next stat/readdir re-fetches it from the MEGA65. */
static void invalidate_cache_parent(const char *path)
{
    char parent[MAX_PATH];
    if (!split_parent(parent, sizeof(parent), path))
        return;
    pthread_mutex_lock(&s_cache_mutex);
    for (int i = 0; i < MAX_CACHED_DIRS; i++) {
        if (s_cache_slots[i].valid &&
            strcmp(s_cache_slots[i].path, parent) == 0) {
            s_cache_slots[i].valid = false;
            break;
        }
    }
    pthread_mutex_unlock(&s_cache_mutex);
}

/* ------------------------------------------------------------------- *
 * FUSE callback implementations
 * ------------------------------------------------------------------- */

/* Open the FTP connection once the filesystem is mounted. */
static void *fs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    (void)cfg;
    printf("FUSE initialized. Connecting to MEGA65 via ethernet...\n");
    if (m65ftp_init() != 0) {
        fprintf(stderr, "[ERROR] Failed to connect to MEGA65 via ethernet.\n");
    } else {
        printf("Connected to MEGA65 successfully.\n");
    }
    return NULL;
}

/* Close the FTP connection when the filesystem is unmounted. */
static void fs_destroy(void *private_data)
{
    (void)private_data;
    printf("Unmounting filesystem. Disconnecting from MEGA65...\n");
    m65ftp_finish();
}

/* stat(2): serve the root directory locally, then the directory cache,
 * falling back to a direct FTP query for uncached paths. */
static int fs_getattr(const char *path, struct stat *stbuf,
                      struct fuse_file_info *fi)
{
    (void)fi;

    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;

    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | DIR_MODE;
        stbuf->st_nlink = DIR_NLINK;
        stbuf->st_size = DIR_SIZE;
        stbuf->st_blocks = DIR_BLOCKS;
        time_t now = time(NULL);
        stbuf->st_atim.tv_sec = now;
        stbuf->st_mtim.tv_sec = now;
        stbuf->st_ctim.tv_sec = now;
        return 0;
    }

    unsigned int file_size = 0;
    int is_dir = 0;
    time_t mtime = 0;
    time_t now = time(NULL);

    pthread_mutex_lock(&s_cache_mutex);
    int found = stat_from_cache(path, &file_size, &is_dir, &mtime, now);
    pthread_mutex_unlock(&s_cache_mutex);

    if (found != 0) {
        if (m65ftp_stat(path, &file_size, &is_dir, &mtime) != 0)
            return -ENOENT;
    }

    if (is_dir) {
        stbuf->st_mode = S_IFDIR | DIR_MODE;
        stbuf->st_nlink = DIR_NLINK;
        stbuf->st_size = DIR_SIZE;
        stbuf->st_blocks = DIR_BLOCKS;
    } else {
        stbuf->st_mode = S_IFREG | FILE_MODE;
        stbuf->st_nlink = FILE_NLINK;
        stbuf->st_size = file_size;
        stbuf->st_blocks = (stbuf->st_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    }
    stbuf->st_atim.tv_sec = mtime;
    stbuf->st_mtim.tv_sec = mtime;
    stbuf->st_ctim.tv_sec = mtime;
    return 0;
}

/* readdir(3): emit "." and "..", then the cached listing for path. */
static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void)fi;
    (void)flags;

    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;

    struct stat st_dot, st_ddot;
    memset(&st_dot, 0, sizeof(st_dot));
    memset(&st_ddot, 0, sizeof(st_ddot));

    st_dot.st_mode = S_IFDIR | DIR_MODE;
    st_dot.st_nlink = DIR_NLINK;
    st_ddot.st_mode = S_IFDIR | DIR_MODE;
    st_ddot.st_nlink = DIR_NLINK;

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
            st.st_mode = cache->entries[i].is_dir
                ? (S_IFDIR | DIR_MODE)
                : (S_IFREG | FILE_MODE);
            st.st_nlink = cache->entries[i].is_dir ? DIR_NLINK : FILE_NLINK;
            st.st_size = cache->entries[i].size;
            st.st_mtim.tv_sec = cache->entries[i].mtime;
            st.st_blocks = (st.st_size + SECTOR_SIZE - 1) / SECTOR_SIZE;

            if (filler(buf, cache->entries[i].name, &st, pos + 1, 0) != 0)
                break;
        }
        pos++;
    }

    return 0;
}

/* open(2): hand the file descriptor to a read-only or read/write FTP
 * handle depending on the requested access mode. */
static int fs_open(const char *path, struct fuse_file_info *fi)
{
    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;

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

/* create(3): make a new file and invalidate its parent listing. */
static int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;

    int fd = m65ftp_create(path);
    if (fd < 0)
        return -EIO;

    fi->fh = (uint64_t)(intptr_t)(size_t)fd;
    invalidate_cache_parent(path);
    return 0;
}

/* write(2): forward to the FTP pwrite on the open handle. */
static int fs_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    int fd = (int)(intptr_t)(size_t)fi->fh;
    ssize_t res = m65ftp_pwrite(fd, (const unsigned char *)buf, size, offset);
    if (res < 0)
        return -EIO;
    return (int)res;
}

/* read(2): forward to the FTP pread on the open handle. */
static int fs_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    (void)path;

    int fd = (int)(intptr_t)(size_t)fi->fh;
    ssize_t res = m65ftp_pread(fd, (unsigned char *)buf, size, offset);
    if (res < 0)
        return -EIO;

    return (int)res;
}

/* release(3): close the FTP handle. */
static int fs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    int fd = (int)(intptr_t)(size_t)fi->fh;
    m65ftp_close(fd);
    return 0;
}

/* flush(2): push any buffered write data to the SD card. */
static int fs_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    int fd = (int)(intptr_t)(size_t)fi->fh;
    if (m65ftp_fsync(fd) != 0)
        return -EIO;
    return 0;
}

/* fsync(2): same as flush, synchronize file data to the SD card. */
static int fs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)datasync;
    int fd = (int)(intptr_t)(size_t)fi->fh;
    if (m65ftp_fsync(fd) != 0)
        return -EIO;
    return 0;
}

/* fsync(3) on a directory: synchronize everything to the SD card. */
static int fs_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)datasync;
    (void)fi;
    if (m65ftp_sync() != 0)
        return -EIO;
    return 0;
}

/* access(2): verify that path exists on the MEGA65. */
static int fs_access(const char *path, int mask)
{
    (void)mask;
    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;

    unsigned int file_size;
    int is_dir;
    time_t mtime;
    if (m65ftp_stat(path, &file_size, &is_dir, &mtime) != 0)
        return -ENOENT;
    return 0;
}

/* truncate(2): change the length of the open file on the SD card. */
static int fs_truncate(const char *path, off_t length,
                       struct fuse_file_info *fi)
{
    (void)path;
    int fd = (int)(intptr_t)(size_t)fi->fh;
    if (m65ftp_ftruncate(fd, length) != 0)
        return -EIO;
    return 0;
}

/* utimensat(2): set the modification/access times on the MEGA65. */
static int fs_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi)
{
    (void)fi;
    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;

    if (m65ftp_utimens(path, tv) != 0)
        return -ENOENT;
    return 0;
}

/* mkdir(2): create a directory and invalidate its parent listing. */
static int fs_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;
    if (m65ftp_mkdir(path) != 0)
        return -EIO;
    invalidate_cache_parent(path);
    return 0;
}

/* rmdir(2): remove an empty directory and invalidate its parent listing. */
static int fs_rmdir(const char *path)
{
    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;
    int ret = m65ftp_rmdir(path);
    if (ret != 0)
        return -ENOTEMPTY;
    invalidate_cache_parent(path);
    return 0;
}

/* unlink(2): remove a file and invalidate its parent listing. */
static int fs_unlink(const char *path)
{
    if (strlen(path) >= MAX_PATH)
        return -ENAMETOOLONG;
    int ret = m65ftp_unlink(path);
    if (ret != 0)
        return -EISDIR;
    invalidate_cache_parent(path);
    return 0;
}

/* rename(2): move from -> to, invalidating both parent listings. */
static int fs_rename(const char *from, const char *to, unsigned int flags)
{
    if (flags != 0)
        return -EINVAL;
    if (strcmp(from, to) == 0)
        return 0;
    if (strlen(from) >= MAX_PATH || strlen(to) >= MAX_PATH)
        return -ENAMETOOLONG;
    if (m65ftp_rename(from, to) != 0)
        return -EIO;
    invalidate_cache_parent(from);
    invalidate_cache_parent(to);
    return 0;
}

/* statfs(3): report the partition geometry plus usage estimated from the
 * cached directory listings. */
static int fs_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    memset(stbuf, 0, sizeof(struct statvfs));

    stbuf->f_frsize = STATFS_FRSIZE;
    stbuf->f_bsize = STATFS_FRSIZE;

    struct m65ftp_partition parts[4];
    unsigned long long total_bytes = FALLBACK_TOTAL_BYTES;

    if (m65ftp_mbrinfo(parts) > 0) {
        /* Partition 0 sector count, SECTOR_SIZE bytes per sector. */
        total_bytes = (unsigned long long)parts[0].sector_count * SECTOR_SIZE;
    }

    fsblkcnt_t total_blocks = total_bytes / STATFS_FRSIZE;

    unsigned long long total_used_bytes = 0;
    for (int i = 0; i < MAX_CACHED_DIRS; i++) {
        if (s_cache_slots[i].valid) {
            for (int j = 0; j < s_cache_slots[i].entry_count; j++) {
                if (s_cache_slots[i].entries[j].valid) {
                    total_used_bytes += s_cache_slots[i].entries[j].size;
                }
            }
        }
    }

    fsblkcnt_t used_blocks =
        (total_used_bytes + STATFS_FRSIZE - 1) / STATFS_FRSIZE;
    fsblkcnt_t free_blocks =
        (total_blocks > used_blocks) ? (total_blocks - used_blocks) : 0;

    stbuf->f_blocks = total_blocks;
    stbuf->f_bfree = free_blocks;
    stbuf->f_bavail = free_blocks;
    stbuf->f_files = STATFS_FILES;
    stbuf->f_ffree = STATFS_FREE_FILES;
    stbuf->f_namemax = STATFS_NAME_MAX;

    return 0;
}

/* ------------------------------------------------------------------- *
 * FUSE operations table
 * ------------------------------------------------------------------- */

static struct fuse_operations fs_oper = {
    .init     = fs_init,
    .destroy  = fs_destroy,
    .getattr  = fs_getattr,
    .readdir  = fs_readdir,
    .open     = fs_open,
    .read     = fs_read,
    .write    = fs_write,
    .create   = fs_create,
    .flush    = fs_flush,
    .release  = fs_release,
    .fsync    = fs_fsync,
    .fsyncdir = fs_fsyncdir,
    .access   = fs_access,
    .truncate = fs_truncate,
    .utimens  = fs_utimens,
    .mkdir    = fs_mkdir,
    .rmdir    = fs_rmdir,
    .unlink   = fs_unlink,
    .rename   = fs_rename,
    .statfs   = fs_statfs,
};

/* ------------------------------------------------------------------- *
 * Entry point
 * ------------------------------------------------------------------- */

static char s_mountpoint[MOUNTPOINT_SIZE] = {0};

/* Unmount the given mountpoint via fusermount(1).
 *
 * Uses fork()+execlp() rather than system(): system() runs the mountpoint
 * through a shell, so an unescaped path becomes a command-injection vector,
 * and system()'s internal shell spawn is not async-signal-safe -- unsafe to
 * call from signal_handler() below. fork()/execlp()/waitpid() are on the
 * POSIX async-signal-safe list (or, for execlp, close enough in practice
 * for a fixed, non-attacker-controlled command name), and passing the
 * mountpoint as an argv element rather than shell text means it is never
 * interpreted by a shell at all. */
static void cleanup_mount(void)
{
    if (!s_mountpoint[0])
        return;

    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("fusermount", "fusermount", "-u", s_mountpoint, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

/* Signal handler: unmount and exit immediately. */
static void signal_handler(int sig)
{
    (void)sig;
    cleanup_mount();
    _exit(1);
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc - 1; i++) {
        if (argv[i][0] != '-') {
            strncpy(s_mountpoint, argv[i], sizeof(s_mountpoint) - 1);
            s_mountpoint[sizeof(s_mountpoint) - 1] = 0;
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
