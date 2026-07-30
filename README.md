# mega65fs — FUSE filesystem for MEGA65 SD card over ethernet

Mount a MEGA65's SD card as a local directory via ethernet (ETHLOAD.M65
must be running on the machine).  Uses the MEGA65-FTP protocol to talk to
the MEGA65's built-in FTP-over-ethernet loader.

## Dependencies

- FUSE 3.x (`libfuse3-dev` / `fuse3`)
- libusb 1.0 (`libusb-1.0-0-dev`)
- readline (`libreadline-dev`)
- GCC, make, pkg-config

## Build

    make

Produces `bin/mega65fs`.

## Usage

    ./bin/mega65fs /mnt/mega65 -f

Foreground (`-f`) is recommended so you can see diagnostics.  The first
connection attempt to the MEGA65 appears in the output.

When the filesystem is no longer needed, unmount with:

    fusermount -u /mnt/mega65

### Defrag utility

    ./bin/mega65fs --defrag [path]

Scans the SD card and rewrites fragmented files into contiguous clusters.
This is run automatically when files are written through the FUSE mount
(writeback buffering ensures newly created files are always contiguous).

### Notes

- All file operations are single-threaded (FUSE `-s` equivalent) — the
  MEGA65-side FTP helper is not re-entrant.
- Directory listing is cached for 5 seconds.
- Writeback buffering: new files are accumulated in host memory and
  written to the SD card as one contiguous block when closed.

## License

The MEGA65-FTP library (`libm65ftp/`) is developed as part of the
[MEGA65](https://mega65.org) project.  This FUSE wrapper is distributed
under the same terms as the MEGA65 tools.
