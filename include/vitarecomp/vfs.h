/* SPDX-License-Identifier: MIT */
#ifndef VITARECOMP_VFS_H
#define VITARECOMP_VFS_H

#include "runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VITA_VFS_MOUNT_READ_ONLY = 1u << 0
};

enum {
    VITA_VFS_OPEN_READ = 1u << 0,
    VITA_VFS_OPEN_WRITE = 1u << 1,
    VITA_VFS_OPEN_CREATE = 1u << 2,
    VITA_VFS_OPEN_TRUNCATE = 1u << 3,
    VITA_VFS_OPEN_APPEND = 1u << 4
};

typedef enum vita_vfs_node_type {
    VITA_VFS_NODE_NONE,
    VITA_VFS_NODE_FILE,
    VITA_VFS_NODE_DIRECTORY
} vita_vfs_node_type;

typedef struct vita_vfs_stat {
    vita_vfs_node_type type;
    uint64_t size;
    uint64_t modified_unix_us;
    uint32_t mount_flags;
} vita_vfs_stat;

typedef struct vita_vfs_file vita_vfs_file;
typedef struct vita_vfs_dir vita_vfs_dir;

int vita_vfs_mount(const char *prefix, const char *host_root, uint32_t flags);
int vita_vfs_mount_psarc(const char *prefix, const char *archive_path);
void vita_vfs_unmount_all(void);

/* Resolves a directory-backed path. Existing paths are canonicalized before
 * the beneath-root check, so symlinks and Windows junctions cannot escape. */
int vita_vfs_resolve(const char *guest_path, uint32_t open_flags,
                     char *host_path, size_t host_path_capacity);

int vita_vfs_open(const char *guest_path, uint32_t flags, vita_vfs_file **out);
int64_t vita_vfs_read(vita_vfs_file *file, void *dst, size_t size);
int64_t vita_vfs_write(vita_vfs_file *file, const void *src, size_t size);
int vita_vfs_seek(vita_vfs_file *file, int64_t offset, int whence,
                  uint64_t *position);
int vita_vfs_file_stat(vita_vfs_file *file, vita_vfs_stat *out);
void vita_vfs_close(vita_vfs_file *file);

int vita_vfs_stat_path(const char *guest_path, vita_vfs_stat *out);
int vita_vfs_remove(const char *guest_path);
int vita_vfs_rename(const char *from, const char *to);

int vita_vfs_opendir(const char *guest_path, vita_vfs_dir **out);
int vita_vfs_readdir(vita_vfs_dir *dir, char *name, size_t capacity,
                     vita_vfs_stat *stat);
void vita_vfs_closedir(vita_vfs_dir *dir);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_VFS_H */
