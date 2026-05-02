#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
_off64_t lseek64(int __filedes, _off64_t __offset, int __whence);

#include "iso_cnf.h"

#define SECTOR_SIZE     2048
#define TOC_LBA         16
#define SYSTEM_CNF_NAME "SYSTEM.CNF;1"

struct dir_toc_entry
{
    short length;
    uint32_t fileLBA;         // 2
    uint32_t fileLBA_bigend;  // 6
    uint32_t fileSize;        // 10
    uint32_t fileSize_bigend; // 14
    uint8_t dateStamp[6];     // 18
    uint8_t reserved1;        // 24
    uint8_t fileProperties;   // 25
    uint8_t reserved2[6];     // 26
    uint8_t filenameLength;   // 32
    char filename[128];       // 33
} __attribute__((packed));

static unsigned char iso_buf[SECTOR_SIZE];

// Reads Primary Volume Descriptor from specified LBA and extracts root directory LBA
static int get_pvd(int fd, uint32_t *lba, int *length);

// Retrieves SYSTEM.CNF TOC entry using specified root directory TOC
static struct dir_toc_entry *get_toc_entry(int fd, uint32_t toc_lba, int toc_len);

// Locate a named entry (file or directory) in an ISO9660 directory.
// Returns a pointer into iso_buf on success, NULL on failure.
// On success, *out_lba and *out_size are populated.
static int find_entry(int fd, uint32_t dir_lba, int dir_len, const char *name,
                      uint32_t *out_lba, uint32_t *out_size, int *out_is_dir);

// Reads SYSTEM.CNF from ISO image and copies it into given buffer
int read_system_cnf(const char *path, char *system_cnf_data, int bufSize)
{
    // Open ISO
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("ERROR: Failed to open file: %d\n", fd);
        return fd;
    }

    // Get location of root directory entry
    uint32_t root_lba = 0;
    int root_len = 0;
    if (get_pvd(fd, &root_lba, &root_len) != 0) {
        printf("ERROR: Failed to parse ISO PVD\n");
        close(fd);
        return -ENOENT;
    }

    // Get SYSTEM.CNF entry
    struct dir_toc_entry *toc_entry = get_toc_entry(fd, root_lba, root_len);
    if (toc_entry == NULL) {
        printf("ERROR: Failed to find SYSTEM.CNF\n");
        close(fd);
        return -EIO;
    }

    // Seek to SYSTEM.CNF location and read file contents
    int64_t res = lseek64(fd, (int64_t)toc_entry->fileLBA * SECTOR_SIZE, SEEK_SET);
    if (res < 0) {
        printf("ERROR: Failed to seek to SYSTEM.CNF\n");
        close(fd);
        return errno;
    }

    res = read(fd, system_cnf_data, bufSize);
    if ((res < toc_entry->length) || (res < bufSize)) {
        res = -EIO;
    } else
        res = 0;

    close(fd);
    return 0;
}

// Reads Primary Volume Descriptor from specified LBA and extracts root directory LBA
static int get_pvd(int fd, uint32_t *lba, int *length)
{
    // Seek to PVD LBA
    int64_t res = lseek64(fd, (int64_t)TOC_LBA * SECTOR_SIZE, SEEK_SET);
    if (res < 0) {
        return -EIO;
    }
    // Read the sector
    if (read(fd, iso_buf, SECTOR_SIZE) == SECTOR_SIZE) {
        // Make sure the sector contains PVD (type code 1, identifier CD001)
        if ((iso_buf[0x00] == 1) && (!memcmp(&iso_buf[0x01], "CD001", 5))) {
            // Read root directory entry and get LBA and length
            struct dir_toc_entry *toc_entry_ptr = (struct dir_toc_entry *)&iso_buf[0x9c];
            *lba = toc_entry_ptr->fileLBA;
            *length = toc_entry_ptr->length;
            return 0;
        } else {
            return -EINVAL;
        }
    }
    return -EIO;
}

// Retrieves SYSTEM.CNF TOC entry using specified root directory TOC
static struct dir_toc_entry *get_toc_entry(int fd, uint32_t toc_lba, int toc_len)
{
    // Read TOC entries
    int64_t res = 0;
    while (toc_len > 0) {
        // Seek to next LBA
        res = lseek64(fd, (int64_t)toc_lba * SECTOR_SIZE, SEEK_SET);
        if (res < 0) {
            return NULL;
        }
        // Read the sector
        if (read(fd, iso_buf, SECTOR_SIZE) != SECTOR_SIZE) {
            return NULL;
        }

        // Read directory entries until the end of sector
        int tocPos = 0;
        struct dir_toc_entry *toc_entry_ptr;
        do {
            toc_entry_ptr = (struct dir_toc_entry *)&iso_buf[tocPos];

            if (toc_entry_ptr->length == 0)
                break;

            if (toc_entry_ptr->filenameLength && !strcmp(SYSTEM_CNF_NAME, toc_entry_ptr->filename)) {
                // File has been found
                return toc_entry_ptr;
            }
            // Advance to the next entry
            tocPos += (toc_entry_ptr->length << 16) >> 16;
        } while (tocPos < 2016);

        // Get next sector LBA
        toc_len -= SECTOR_SIZE;
        toc_lba++;
    }

    return NULL;
}

// Case-insensitive comparison of ISO9660 filename (in TOC) against a target.
// ISO9660 filenames are uppercase, but be lenient with input.
static int iso_name_eq(const char *toc_name, int toc_namelen, const char *target)
{
    int i;
    int target_len = (int)strlen(target);
    if (toc_namelen != target_len)
        return 0;
    for (i = 0; i < toc_namelen; i++) {
        char a = toc_name[i];
        char b = target[i];
        if (a >= 'a' && a <= 'z') a = a - 'a' + 'A';
        if (b >= 'a' && b <= 'z') b = b - 'a' + 'A';
        if (a != b)
            return 0;
    }
    return 1;
}

// Walk one directory looking for an entry by name.
// On success, returns 0 and populates out_lba/out_size/out_is_dir.
// Returns -ENOENT if not found, other negative errno on I/O error.
static int find_entry(int fd, uint32_t dir_lba, int dir_len, const char *name,
                      uint32_t *out_lba, uint32_t *out_size, int *out_is_dir)
{
    while (dir_len > 0) {
        int64_t res = lseek64(fd, (int64_t)dir_lba * SECTOR_SIZE, SEEK_SET);
        if (res < 0)
            return -EIO;
        if (read(fd, iso_buf, SECTOR_SIZE) != SECTOR_SIZE)
            return -EIO;

        int tocPos = 0;
        struct dir_toc_entry *e;
        do {
            e = (struct dir_toc_entry *)&iso_buf[tocPos];
            if (e->length == 0)
                break;

            // Skip the "." (00) and ".." (01) records: filenameLength == 1
            // and filename[0] is 0x00 or 0x01.
            if (e->filenameLength == 1 && (e->filename[0] == 0x00 || e->filename[0] == 0x01)) {
                tocPos += (e->length << 16) >> 16;
                continue;
            }

            if (e->filenameLength > 0 &&
                iso_name_eq(e->filename, e->filenameLength, name)) {
                *out_lba    = e->fileLBA;
                *out_size   = e->fileSize;
                *out_is_dir = (e->fileProperties & 0x02) ? 1 : 0;
                return 0;
            }

            tocPos += (e->length << 16) >> 16;
        } while (tocPos < 2016);

        dir_len -= SECTOR_SIZE;
        dir_lba++;
    }
    return -ENOENT;
}

int read_file_from_iso(const char *path, const char *dir_name, const char *file_name,
                       void **out_buf, uint32_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;

    uint32_t cur_lba = 0;
    int      cur_len = 0;
    if (get_pvd(fd, &cur_lba, &cur_len) != 0) {
        close(fd);
        return -ENOENT;
    }

    // Descend into requested subdirectory if any
    if (dir_name != NULL && dir_name[0] != '\0') {
        uint32_t sub_lba = 0;
        uint32_t sub_size = 0;
        int      is_dir = 0;
        int rc = find_entry(fd, cur_lba, cur_len, dir_name, &sub_lba, &sub_size, &is_dir);
        if (rc != 0) {
            close(fd);
            return rc;
        }
        if (!is_dir) {
            close(fd);
            return -ENOTDIR;
        }
        cur_lba = sub_lba;
        cur_len = (int)sub_size;
    }

    // Find the file within the (possibly subdirectory) directory
    uint32_t file_lba = 0;
    uint32_t file_size = 0;
    int      is_dir = 0;
    int rc = find_entry(fd, cur_lba, cur_len, file_name, &file_lba, &file_size, &is_dir);
    if (rc != 0) {
        close(fd);
        return rc;
    }
    if (is_dir) {
        close(fd);
        return -EISDIR;
    }
    if (file_size == 0) {
        close(fd);
        return -EIO;
    }

    void *buf = malloc(file_size);
    if (buf == NULL) {
        close(fd);
        return -ENOMEM;
    }

    int64_t sres = lseek64(fd, (int64_t)file_lba * SECTOR_SIZE, SEEK_SET);
    if (sres < 0) {
        free(buf);
        close(fd);
        return -EIO;
    }

    // Read the file. Round up to sector boundary because some FS layers
    // require sector-aligned I/O.
    uint32_t aligned_size = (file_size + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
    void *aligned_buf = malloc(aligned_size);
    if (aligned_buf == NULL) {
        free(buf);
        close(fd);
        return -ENOMEM;
    }

    int total = 0;
    while (total < (int)aligned_size) {
        int n = read(fd, (char *)aligned_buf + total, aligned_size - total);
        if (n <= 0)
            break;
        total += n;
    }
    close(fd);

    if (total < (int)file_size) {
        free(aligned_buf);
        free(buf);
        return -EIO;
    }

    memcpy(buf, aligned_buf, file_size);
    free(aligned_buf);

    *out_buf  = buf;
    *out_size = file_size;
    return 0;
}
