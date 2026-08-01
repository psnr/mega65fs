#ifndef M65FTP_LIB_H
#define M65FTP_LIB_H

/*
 * m65ftp_lib.h — local API over the vendored MEGA65-FTP library.
 *
 * All functions are thread-safe: they serialise on an internal mutex.
 * Every call returns a negative value on failure unless documented
 * otherwise.
 */

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Partition information
 * ------------------------------------------------------------------- */

struct m65ftp_partition {
    unsigned int start_sector;
    unsigned int sector_count;
    unsigned char type;
};

/*
 * Read the MBR partition table.  Populates partitions[4] and returns the
 * number of non-empty partitions, or -1 on failure.
 */
int m65ftp_mbrinfo(struct m65ftp_partition partitions[4]);

/* ---------------------------------------------------------------------
 * Connection lifecycle
 * ------------------------------------------------------------------- */

/*
 * Establish the ethernet connection to the MEGA65 and mount the SD
 * filesystem.  Safe to call multiple times; only the first call does any
 * work.  Returns 0 on success, -1 on failure.
 */
int m65ftp_init(void);

/*
 * Tear the connection down.  A no-op if never initialised.
 */
void m65ftp_finish(void);

/* ---------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------- */

/*
 * Open an existing file for reading.  Returns a non-negative file
 * descriptor, or -1 on failure.
 */
int m65ftp_open(const char *path);

/*
 * Create a new (empty) file.  Data is buffered in host memory and only
 * written to the SD card, as one contiguous cluster block, when the
 * descriptor is closed.  Returns a non-negative descriptor, or -1.
 */
int m65ftp_create(const char *path);

/*
 * Open an existing file for writing.  Writes go straight to the SD card
 * (no host-memory buffering).  Returns a non-negative descriptor, or -1.
 */
int m65ftp_open_writable(const char *path);

/*
 * Close a descriptor, flushing any buffered data to the SD card.
 * Safe to call on an already-closed or invalid descriptor.
 */
void m65ftp_close(int fd);

/* ---------------------------------------------------------------------
 * Read / write
 * ------------------------------------------------------------------- */

/*
 * Read up to count bytes at offset.  Returns the number of bytes read
 * (possibly fewer than requested at end of file), 0 at/after EOF, or -1.
 */
ssize_t m65ftp_pread(int fd, unsigned char *buf, size_t count, off_t offset);

/*
 * Write count bytes at offset.  Returns the number of bytes written, or -1.
 */
ssize_t m65ftp_pwrite(int fd, const unsigned char *buf, size_t count,
                      off_t offset);

/*
 * Truncate (or extend) an open descriptor to length.  The extension path
 * re-allocates the whole file as a single contiguous block.
 * Returns 0 on success, -1 on failure.
 */
int m65ftp_ftruncate(int fd, off_t length);

/*
 * Flush an open descriptor's buffered writes to the SD card (flush/fsync).
 * Returns 0 on success, -1 on failure.
 */
int m65ftp_fsync(int fd);

/*
 * Flush all pending sector writes to the SD card (fsyncdir support).
 * Returns 0 on success, -1 on failure.
 */
int m65ftp_sync(void);

/* ---------------------------------------------------------------------
 * Metadata
 * ------------------------------------------------------------------- */

/*
 * Look up a path.  Any of file_size / is_dir / mtime may be NULL.
 * Returns 0 on success, -1 if the entry does not exist.
 */
int m65ftp_stat(const char *path, unsigned int *file_size, int *is_dir,
                time_t *mtime);

/*
 * Iterate a directory, invoking cb for every entry until cb returns
 * non-zero.  Returns 0 on success, -1 on failure.
 */
int m65ftp_readdir(const char *path,
                   int (*cb)(const char *name, unsigned int file_size,
                             int is_dir, time_t mtime, void *ctx),
                   void *ctx);

/*
 * Set a file's access and modification times.  tv follows the
 * utimensat(2) convention (UTIME_NOW / UTIME_OMIT supported).
 * Returns 0 on success, -1 on failure.
 */
int m65ftp_utimens(const char *path, const struct timespec tv[2]);

/* ---------------------------------------------------------------------
 * Directory operations
 * ------------------------------------------------------------------- */

/* Create a directory.  0 / -1. */
int m65ftp_mkdir(const char *path);

/* Remove an empty directory.  0 / -1. */
int m65ftp_rmdir(const char *path);

/* Delete a regular file.  Refuses to delete directories.  0 / -1. */
int m65ftp_unlink(const char *path);

/* Rename a file or directory within the same directory.  0 / -1. */
int m65ftp_rename(const char *oldpath, const char *newpath);

#ifdef __cplusplus
}
#endif

#endif /* M65FTP_LIB_H */
