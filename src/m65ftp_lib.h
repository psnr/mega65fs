#ifndef M65FTP_LIB_H
#define M65FTP_LIB_H

#include <stddef.h>
#include <sys/types.h>

struct m65ftp_partition {
    unsigned int start_sector;
    unsigned int sector_count;
    unsigned char type;
};

int m65ftp_mbrinfo(struct m65ftp_partition partitions[4]);

int m65ftp_init(void);
void m65ftp_finish(void);
int m65ftp_is_connected(void);

int m65ftp_open(const char *path);
void m65ftp_close(int fd);
ssize_t m65ftp_pread(int fd, unsigned char *buf, size_t count, off_t offset);

int m65ftp_create(const char *path);
ssize_t m65ftp_pwrite(int fd, const unsigned char *buf, size_t count, off_t offset);

int m65ftp_stat(const char *path, unsigned int *file_size, int *is_dir, time_t *mtime);

int m65ftp_readdir(const char *path,
    int (*cb)(const char *name, unsigned int file_size, int is_dir, time_t mtime, void *ctx),
    void *ctx);

/* Fragmentation check for defrag mode */
int m65ftp_is_fragmented(const char *path);

/* Set file timestamps (touch/utimensat) */
int m65ftp_utimens(const char *path, const struct timespec tv[2]);

/* Directory creation and deletion */
int m65ftp_mkdir(const char *path);
int m65ftp_rmdir(const char *path);
int m65ftp_unlink(const char *path);

/* Open an existing file for writing */
int m65ftp_open_writable(const char *path);

/* Truncate an open file (fd-based) */
int m65ftp_ftruncate(int fd, off_t length);

/* Rename a file (same directory, or across directories) */
int m65ftp_rename(const char *oldpath, const char *newpath);

#endif
