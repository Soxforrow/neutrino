#ifndef ISO_CNF_H
#define ISO_CNF_H

#include <stdint.h>
#include <sys/types.h>

// Reads SYSTEM.CNF from ISO image and copies it into given buffer
int read_system_cnf(const char *path, char *system_cnf_data, int bufSize);

// Reads an arbitrary file from inside an ISO image into a malloc'd buffer.
//
// path        : path to the ISO file (e.g. "udpfs0:games/Black.iso")
// dir_name    : top-level directory name in the ISO (e.g. "IOP")
//               pass NULL or "" to look in the root directory
// file_name   : ISO9660 file name with version (e.g. "GTFSCDVD.IRX;1")
// out_buf     : (out) on success, set to a newly malloc'd buffer holding
//               file contents. caller is responsible for freeing.
// out_size    : (out) on success, set to the file size in bytes.
//
// Returns 0 on success, negative errno on failure.
int read_file_from_iso(const char *path, const char *dir_name, const char *file_name,
                       void **out_buf, uint32_t *out_size);

#endif
