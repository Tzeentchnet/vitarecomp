/* SPDX-License-Identifier: MIT */
/* SceIofilemgr over the title-neutral VFS. */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/vfs.h"
#include "hle_vfs.h"

#include <stdio.h>
#include <string.h>
#include <threads.h>

#define SCE_ERROR_ERRNO_EPERM  UINT32_C(0x80010001)
#define SCE_ERROR_ERRNO_ENOENT UINT32_C(0x80010002)
#define SCE_ERROR_ERRNO_EIO    UINT32_C(0x80010005)
#define SCE_ERROR_ERRNO_EBADF  UINT32_C(0x80010009)
#define SCE_ERROR_ERRNO_EINVAL UINT32_C(0x80010016)

#define SCE_O_RDONLY 1u
#define SCE_O_WRONLY 2u
#define SCE_O_RDWR   3u
#define SCE_O_APPEND 0x100u
#define SCE_O_CREAT  0x200u
#define SCE_O_TRUNC  0x400u

#define FD_FIRST UINT32_C(0x40200000)
#define MAX_FDS 64
#define DD_FIRST UINT32_C(0x40300000)
#define MAX_DDS 32

typedef struct fd_entry {
    int used;
    int console;
    vita_vfs_file *file;
} fd_entry;

typedef struct dd_entry {
    int used;
    vita_vfs_dir *dir;
} dd_entry;

static once_flag g_once = ONCE_FLAG_INIT;
static mtx_t g_lock;
static fd_entry g_fds[MAX_FDS];
static dd_entry g_dds[MAX_DDS];

static void io_once(void) {
    (void)mtx_init(&g_lock, mtx_plain);
}

static uint32_t sce_error(int result) {
    switch (result) {
    case VITA_ERROR_READ_ONLY:
    case VITA_ERROR_TRAVERSAL:
        return SCE_ERROR_ERRNO_EPERM;
    case VITA_ERROR_NOT_FOUND:
        return SCE_ERROR_ERRNO_ENOENT;
    case VITA_ERROR_INVALID_ARGUMENT:
    case VITA_ERROR_LIMIT:
    case VITA_ERROR_OUT_OF_BOUNDS:
        return SCE_ERROR_ERRNO_EINVAL;
    default:
        return SCE_ERROR_ERRNO_EIO;
    }
}

static uint32_t vfs_flags(uint32_t flags) {
    uint32_t out = 0;
    if ((flags & SCE_O_RDWR) == SCE_O_RDWR)
        out |= VITA_VFS_OPEN_READ | VITA_VFS_OPEN_WRITE;
    else if (flags & SCE_O_WRONLY)
        out |= VITA_VFS_OPEN_WRITE;
    else
        out |= VITA_VFS_OPEN_READ;
    if (flags & SCE_O_APPEND) out |= VITA_VFS_OPEN_APPEND;
    if (flags & SCE_O_CREAT) out |= VITA_VFS_OPEN_CREATE;
    if (flags & SCE_O_TRUNC) out |= VITA_VFS_OPEN_TRUNCATE;
    return out;
}

static int guest_path(uint32_t address, char *path, size_t capacity) {
    size_t length = 0;
    return vita_mem_read_cstr(address, path, capacity, &length) == VITA_OK;
}

static int fd_index(uint32_t descriptor) {
    if (descriptor < FD_FIRST || descriptor >= FD_FIRST + MAX_FDS) return -1;
    int index = (int)(descriptor - FD_FIRST);
    return g_fds[index].used ? index : -1;
}

static int dd_index(uint32_t descriptor) {
    if (descriptor < DD_FIRST || descriptor >= DD_FIRST + MAX_DDS) return -1;
    int index = (int)(descriptor - DD_FIRST);
    return g_dds[index].used ? index : -1;
}

void vita_io_mount(const char *prefix, const char *host_dir) {
    (void)vita_vfs_mount(prefix, host_dir, VITA_VFS_MOUNT_READ_ONLY);
}

int vita_io_resolve_for(const char *guest, int writes,
                        char *host, size_t capacity) {
    uint32_t flags = writes
        ? VITA_VFS_OPEN_WRITE | VITA_VFS_OPEN_CREATE
        : VITA_VFS_OPEN_READ;
    return vita_vfs_resolve(guest, flags, host, capacity) == VITA_OK;
}

int vita_io_resolve(const char *guest, char *host, size_t capacity) {
    return vita_io_resolve_for(guest, 0, host, capacity);
}

void vita_hle_sceIoOpen(void) {
    char path[1024];
    if (!guest_path(r0, path, sizeof(path))) {
        r0 = SCE_ERROR_ERRNO_EINVAL;
        return;
    }
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    int index;
    for (index = 0; index < MAX_FDS && g_fds[index].used; index++) {}
    if (index == MAX_FDS) {
        mtx_unlock(&g_lock);
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }

    fd_entry entry;
    memset(&entry, 0, sizeof(entry));
    int result = VITA_OK;
    if (strncmp(path, "tty", 3) == 0) {
        entry.console = path[3] == '1' ? 2 : 1;
    } else {
        result = vita_vfs_open(path, vfs_flags(r1), &entry.file);
    }
    if (result != VITA_OK) {
        mtx_unlock(&g_lock);
        r0 = sce_error(result);
        return;
    }
    entry.used = 1;
    g_fds[index] = entry;
    mtx_unlock(&g_lock);
    r0 = FD_FIRST + (uint32_t)index;
}

void vita_hle_sceIoClose(void) {
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    int index = fd_index(r0);
    if (index < 0) {
        mtx_unlock(&g_lock);
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }
    vita_vfs_file *file = g_fds[index].file;
    memset(&g_fds[index], 0, sizeof(g_fds[index]));
    mtx_unlock(&g_lock);
    vita_vfs_close(file);
    r0 = 0;
}

void vita_hle_sceIoRead(void) {
    uint32_t descriptor = r0, address = r1, size = r2;
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    int index = fd_index(descriptor);
    vita_vfs_file *file = index >= 0 ? g_fds[index].file : NULL;
    int console = index >= 0 ? g_fds[index].console : 0;
    mtx_unlock(&g_lock);
    if (index < 0) {
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }
    if (!size || console) {
        r0 = 0;
        return;
    }
    void *buffer = NULL;
    if (vita_mem_range(address, size, &buffer) != VITA_OK) {
        r0 = SCE_ERROR_ERRNO_EINVAL;
        return;
    }
    int64_t read = vita_vfs_read(file, buffer, size);
    if (read < 0) {
        r0 = sce_error((int)read);
        return;
    }
    if (read) vita_mem_mark_written(address, (size_t)read);
    r0 = (uint32_t)read;
}

void vita_hle_sceIoWrite(void) {
    uint32_t descriptor = r0, address = r1, size = r2;
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    int index = fd_index(descriptor);
    vita_vfs_file *file = index >= 0 ? g_fds[index].file : NULL;
    int console = index >= 0 ? g_fds[index].console : 0;
    mtx_unlock(&g_lock);
    if (index < 0) {
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }
    if (!size) {
        r0 = 0;
        return;
    }
    void *buffer = NULL;
    if (vita_mem_range(address, size, &buffer) != VITA_OK) {
        r0 = SCE_ERROR_ERRNO_EINVAL;
        return;
    }
    if (console) {
        FILE *stream = console == 2 ? stderr : stdout;
        size_t written = fwrite(buffer, 1, size, stream);
        fflush(stream);
        r0 = (uint32_t)written;
        return;
    }
    int64_t written = vita_vfs_write(file, buffer, size);
    r0 = written < 0 ? sce_error((int)written) : (uint32_t)written;
}

static int whence_of(uint32_t whence) {
    return whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
}

static int get_file(uint32_t descriptor, vita_vfs_file **file, int *console) {
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    int index = fd_index(descriptor);
    if (index >= 0) {
        *file = g_fds[index].file;
        *console = g_fds[index].console;
    }
    mtx_unlock(&g_lock);
    return index;
}

void vita_hle_sceIoLseek32(void) {
    vita_vfs_file *file = NULL;
    int console = 0;
    if (get_file(r0, &file, &console) < 0) {
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }
    if (console) {
        r0 = 0;
        return;
    }
    uint64_t position = 0;
    int result = vita_vfs_seek(file, (int32_t)r1, whence_of(r2), &position);
    r0 = result == VITA_OK && position <= UINT32_MAX
       ? (uint32_t)position : sce_error(result);
}

void vita_hle_sceIoLseek(void) {
    vita_vfs_file *file = NULL;
    int console = 0;
    if (get_file(r0, &file, &console) < 0) {
        r0 = SCE_ERROR_ERRNO_EBADF;
        r1 = UINT32_MAX;
        return;
    }
    int64_t offset = (int64_t)(((uint64_t)r3 << 32) | r2);
    uint64_t position = 0;
    int result = console ? VITA_OK
                         : vita_vfs_seek(file, offset,
                                         whence_of(vita_read32(sp)), &position);
    if (result != VITA_OK) {
        r0 = sce_error(result);
        r1 = UINT32_MAX;
    } else {
        r0 = (uint32_t)position;
        r1 = (uint32_t)(position >> 32);
    }
}

static void write_iostat(uint32_t out, const vita_vfs_stat *value) {
    uint8_t zero[88] = {0};
    (void)vita_mem_copy_to_guest(out, zero, sizeof(zero));
    uint32_t mode = value->type == VITA_VFS_NODE_DIRECTORY
                  ? UINT32_C(0x1000) : UINT32_C(0x2000);
    mode |= (value->mount_flags & VITA_VFS_MOUNT_READ_ONLY)
          ? UINT32_C(0x124) : UINT32_C(0x1b6);
    vita_write32(out, mode);
    vita_write32(out + 8, (uint32_t)value->size);
    vita_write32(out + 12, (uint32_t)(value->size >> 32));
}

void vita_hle_sceIoGetstat(void) {
    char path[1024];
    if (!guest_path(r0, path, sizeof(path)) ||
        !vita_mem_ptr(r1, 88)) {
        r0 = SCE_ERROR_ERRNO_EINVAL;
        return;
    }
    vita_vfs_stat value;
    int result = vita_vfs_stat_path(path, &value);
    if (result != VITA_OK) {
        r0 = sce_error(result);
        return;
    }
    write_iostat(r1, &value);
    r0 = 0;
}

void vita_hle_sceIoGetstatByFd(void) {
    uint32_t output = r1;
    vita_vfs_file *file = NULL;
    int console = 0;
    if (get_file(r0, &file, &console) < 0 || !vita_mem_ptr(output, 88)) {
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }
    vita_vfs_stat value;
    memset(&value, 0, sizeof(value));
    value.type = VITA_VFS_NODE_FILE;
    value.mount_flags = console ? VITA_VFS_MOUNT_READ_ONLY : 0;
    int result = console ? VITA_OK : vita_vfs_file_stat(file, &value);
    if (result == VITA_OK) write_iostat(output, &value);
    r0 = result == VITA_OK ? 0 : sce_error(result);
}

void vita_hle_sceIoRename(void) {
    char from[1024], to[1024];
    if (!guest_path(r0, from, sizeof(from)) ||
        !guest_path(r1, to, sizeof(to))) {
        r0 = SCE_ERROR_ERRNO_EINVAL;
        return;
    }
    r0 = sce_error(vita_vfs_rename(from, to));
    if (r0 == sce_error(VITA_OK)) r0 = 0;
}

void vita_hle_sceIoRemove(void) {
    char path[1024];
    if (!guest_path(r0, path, sizeof(path))) {
        r0 = SCE_ERROR_ERRNO_EINVAL;
        return;
    }
    int result = vita_vfs_remove(path);
    r0 = result == VITA_OK ? 0 : sce_error(result);
}

void vita_hle_sceIoSyncByFd(void) {
    vita_vfs_file *file = NULL;
    int console = 0;
    r0 = get_file(r0, &file, &console) < 0 ? SCE_ERROR_ERRNO_EBADF : 0;
    (void)file;
    (void)console;
}

void vita_hle_sceIoDopen(void) {
    char path[1024];
    if (!guest_path(r0, path, sizeof(path))) {
        r0 = SCE_ERROR_ERRNO_EINVAL;
        return;
    }
    vita_vfs_dir *dir = NULL;
    int result = vita_vfs_opendir(path, &dir);
    if (result != VITA_OK) {
        r0 = sce_error(result);
        return;
    }
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    int index;
    for (index = 0; index < MAX_DDS && g_dds[index].used; index++) {}
    if (index == MAX_DDS) {
        mtx_unlock(&g_lock);
        vita_vfs_closedir(dir);
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }
    g_dds[index].used = 1;
    g_dds[index].dir = dir;
    mtx_unlock(&g_lock);
    r0 = DD_FIRST + (uint32_t)index;
}

void vita_hle_sceIoDread(void) {
    uint32_t output = r1;
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    int index = dd_index(r0);
    vita_vfs_dir *dir = index >= 0 ? g_dds[index].dir : NULL;
    mtx_unlock(&g_lock);
    if (index < 0) {
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }
    /* SceIoDirent begins with an 88-byte stat and a 256-byte name. */
    if (!vita_mem_ptr(output, 344)) {
        r0 = SCE_ERROR_ERRNO_EINVAL;
        return;
    }
    char name[256];
    vita_vfs_stat value;
    int result = vita_vfs_readdir(dir, name, sizeof(name), &value);
    if (result == VITA_ERROR_NOT_FOUND) {
        r0 = 0;
        return;
    }
    if (result != VITA_OK) {
        r0 = sce_error(result);
        return;
    }
    write_iostat(output, &value);
    uint8_t zero[256] = {0};
    (void)vita_mem_copy_to_guest(output + 88, zero, sizeof(zero));
    (void)vita_mem_copy_to_guest(output + 88, name, strlen(name) + 1);
    r0 = 1;
}

void vita_hle_sceIoDclose(void) {
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    int index = dd_index(r0);
    if (index < 0) {
        mtx_unlock(&g_lock);
        r0 = SCE_ERROR_ERRNO_EBADF;
        return;
    }
    vita_vfs_dir *dir = g_dds[index].dir;
    memset(&g_dds[index], 0, sizeof(g_dds[index]));
    mtx_unlock(&g_lock);
    vita_vfs_closedir(dir);
    r0 = 0;
}

void vita_io_shutdown(void) {
    call_once(&g_once, io_once);
    mtx_lock(&g_lock);
    fd_entry files[MAX_FDS];
    dd_entry dirs[MAX_DDS];
    memcpy(files, g_fds, sizeof(files));
    memcpy(dirs, g_dds, sizeof(dirs));
    memset(g_fds, 0, sizeof(g_fds));
    memset(g_dds, 0, sizeof(g_dds));
    mtx_unlock(&g_lock);
    for (int i = 0; i < MAX_FDS; i++)
        if (files[i].used) vita_vfs_close(files[i].file);
    for (int i = 0; i < MAX_DDS; i++)
        if (dirs[i].used) vita_vfs_closedir(dirs[i].dir);
}
