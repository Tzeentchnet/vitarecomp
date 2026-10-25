/* SPDX-License-Identifier: MIT */
#include "vitarecomp/vfs.h"

#include "psarc.h"
#include "runtime_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define host_stat _stat64
#define host_stat_t struct __stat64
#define host_seek _fseeki64
#define host_tell _ftelli64
#define PATH_SEP '\\'
#else
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#define host_stat stat
#define host_stat_t struct stat
#define host_seek fseeko
#define host_tell ftello
#define PATH_SEP '/'
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_MOUNTS 16

typedef enum mount_kind {
    MOUNT_DIRECTORY,
    MOUNT_PSARC
} mount_kind;

typedef struct mount_entry {
    int used;
    mount_kind kind;
    char prefix[16];
    uint32_t flags;
    char root[PATH_MAX];
    char canonical_root[PATH_MAX];
    psarc *archive;
} mount_entry;

struct vita_vfs_file {
    mtx_t lock;
    FILE *host;
    psarc_file *archive;
    uint32_t flags;
    uint32_t mount_flags;
    uint64_t position;
    struct vita_vfs_file *next;
};

struct vita_vfs_dir {
    mtx_t lock;
    char root[PATH_MAX];
    char archive_path[PATH_MAX];
    uint32_t mount_flags;
    psarc *archive;
    uint32_t archive_cursor;
#ifdef _WIN32
    intptr_t search;
    struct _finddata_t current;
    int have_current;
#else
    DIR *host;
#endif
    struct vita_vfs_dir *next;
};

typedef struct parsed_path {
    char prefix[16];
    char relative[PATH_MAX];
} parsed_path;

static once_flag g_once = ONCE_FLAG_INIT;
static mtx_t g_lock;
static mount_entry g_mounts[MAX_MOUNTS];
static vita_vfs_file *g_files;
static vita_vfs_dir *g_dirs;

static void vfs_once(void) {
    (void)mtx_init(&g_lock, mtx_plain);
}

static int ascii_equal(char left, char right) {
    return tolower((unsigned char)left) == tolower((unsigned char)right);
}

static int prefix_equal(const char *left, const char *right) {
    while (*left && *right) {
        if (!ascii_equal(*left++, *right++)) return 0;
    }
    return !*left && !*right;
}

static int normalize_prefix(const char *prefix, char out[16]) {
    if (!prefix || !*prefix) return VITA_ERROR_INVALID_ARGUMENT;
    size_t n = 0;
    while (prefix[n] && prefix[n] != ':') {
        unsigned char c = (unsigned char)prefix[n];
        if (!(isalnum(c) || c == '_') || n + 1 >= 16)
            return VITA_ERROR_INVALID_ARGUMENT;
        out[n] = (char)tolower(c);
        n++;
    }
    if (!n) return VITA_ERROR_INVALID_ARGUMENT;
    if (prefix[n] == ':') n++;
    if (prefix[n] != '\0') return VITA_ERROR_INVALID_ARGUMENT;
    out[n - (prefix[n - 1] == ':' ? 1u : 0u)] = '\0';
    return VITA_OK;
}

static int parse_guest_path(const char *guest, parsed_path *out) {
    if (!guest || !out) return VITA_ERROR_INVALID_ARGUMENT;
    const char *colon = strchr(guest, ':');
    if (!colon || colon == guest || strchr(colon + 1, ':'))
        return VITA_ERROR_INVALID_ARGUMENT;
    size_t prefix_length = (size_t)(colon - guest);
    if (prefix_length >= sizeof(out->prefix))
        return VITA_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0; i < prefix_length; i++) {
        unsigned char c = (unsigned char)guest[i];
        if (!(isalnum(c) || c == '_')) return VITA_ERROR_INVALID_ARGUMENT;
        out->prefix[i] = (char)tolower(c);
    }
    out->prefix[prefix_length] = '\0';

    const char *at = colon + 1;
    while (*at == '/' || *at == '\\') at++;
    size_t length = 0;
    while (*at) {
        while (*at == '/' || *at == '\\') at++;
        if (!*at) break;
        const char *component = at;
        while (*at && *at != '/' && *at != '\\') at++;
        size_t component_length = (size_t)(at - component);
        if (component_length == 1 && component[0] == '.') continue;
        if (component_length == 2 && component[0] == '.' &&
            component[1] == '.')
            return VITA_ERROR_TRAVERSAL;
        if (!component_length) continue;
        for (size_t i = 0; i < component_length; i++) {
            unsigned char c = (unsigned char)component[i];
            if (c < 0x20 || c == ':' || c == '\x7f')
                return VITA_ERROR_TRAVERSAL;
        }
        if (length && length + 1 >= sizeof(out->relative))
            return VITA_ERROR_LIMIT;
        if (length) out->relative[length++] = '/';
        if (component_length >= sizeof(out->relative) - length)
            return VITA_ERROR_LIMIT;
        memcpy(out->relative + length, component, component_length);
        length += component_length;
    }
    out->relative[length] = '\0';
    return VITA_OK;
}

static int path_is_beneath(const char *path, const char *root) {
    size_t n = strlen(root);
#ifdef _WIN32
    for (size_t i = 0; i < n; i++)
        if (!path[i] || !ascii_equal(path[i] == '/' ? '\\' : path[i],
                                     root[i] == '/' ? '\\' : root[i]))
            return 0;
#else
    if (strncmp(path, root, n) != 0) return 0;
#endif
    if (!path[n]) return 1;
    return path[n] == '/' || path[n] == '\\' ||
           (n && (root[n - 1] == '/' || root[n - 1] == '\\'));
}

#ifdef _WIN32
static int canonical_existing(const char *path, char *out, size_t capacity) {
    HANDLE handle = CreateFileA(
        path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return VITA_ERROR_NOT_FOUND;
    DWORD length = GetFinalPathNameByHandleA(handle, out, (DWORD)capacity,
                                             FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (!length || length >= capacity) return VITA_ERROR_LIMIT;
    return VITA_OK;
}

static int absolute_path(const char *path, char *out, size_t capacity) {
    return _fullpath(out, path, capacity) ? VITA_OK : VITA_ERROR_INVALID_ARGUMENT;
}

static int is_link_or_reparse(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
static int canonical_existing(const char *path, char *out, size_t capacity) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return VITA_ERROR_NOT_FOUND;
    size_t n = strlen(resolved);
    if (n >= capacity) return VITA_ERROR_LIMIT;
    memcpy(out, resolved, n + 1);
    return VITA_OK;
}

static int absolute_path(const char *path, char *out, size_t capacity) {
    char resolved[PATH_MAX];
    if (path[0] == '/') {
        if (strlen(path) >= capacity) return VITA_ERROR_LIMIT;
        strcpy(out, path);
        return VITA_OK;
    }
    if (!getcwd(resolved, sizeof(resolved))) return VITA_ERROR_IO;
    int written = snprintf(out, capacity, "%s/%s", resolved, path);
    return written >= 0 && (size_t)written < capacity
         ? VITA_OK : VITA_ERROR_LIMIT;
}

static int is_link_or_reparse(const char *path) {
    struct stat value;
    return lstat(path, &value) == 0 && S_ISLNK(value.st_mode);
}
#endif

static int join_candidate(const mount_entry *mount, const char *relative,
                          char *out, size_t capacity) {
    int written = relative[0]
        ? snprintf(out, capacity, "%s%c%s", mount->root, PATH_SEP, relative)
        : snprintf(out, capacity, "%s", mount->root);
    if (written < 0 || (size_t)written >= capacity) return VITA_ERROR_LIMIT;
    for (char *at = out; *at; at++)
        if (*at == '/' || *at == '\\') *at = PATH_SEP;
    return VITA_OK;
}

static int canonical_parent(const char *path, char *out, size_t capacity) {
    char parent[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(parent)) return VITA_ERROR_LIMIT;
    memcpy(parent, path, n + 1);
    while (n && parent[n - 1] != '/' && parent[n - 1] != '\\') n--;
    if (!n) return VITA_ERROR_TRAVERSAL;
    while (n > 1 && (parent[n - 1] == '/' || parent[n - 1] == '\\')) n--;
    parent[n] = '\0';
    return canonical_existing(parent, out, capacity);
}

static int resolve_mount(const mount_entry *mount, const char *relative,
                         uint32_t open_flags, char *out, size_t capacity,
                         int require_exists) {
    if (mount->kind != MOUNT_DIRECTORY) return VITA_ERROR_NOT_FOUND;
    const int writes = (open_flags &
        (VITA_VFS_OPEN_WRITE | VITA_VFS_OPEN_CREATE |
         VITA_VFS_OPEN_TRUNCATE | VITA_VFS_OPEN_APPEND)) != 0;
    if (writes && (mount->flags & VITA_VFS_MOUNT_READ_ONLY))
        return VITA_ERROR_READ_ONLY;

    char candidate[PATH_MAX];
    int result = join_candidate(mount, relative, candidate, sizeof(candidate));
    if (result != VITA_OK) return result;
    char absolute[PATH_MAX];
    result = absolute_path(candidate, absolute, sizeof(absolute));
    if (result != VITA_OK || !path_is_beneath(absolute, mount->root))
        return VITA_ERROR_TRAVERSAL;

    char canonical[PATH_MAX];
    result = canonical_existing(absolute, canonical, sizeof(canonical));
    if (result == VITA_OK) {
        if (!path_is_beneath(canonical, mount->canonical_root))
            return VITA_ERROR_TRAVERSAL;
    } else {
        if (require_exists || !(open_flags & VITA_VFS_OPEN_CREATE))
            return VITA_ERROR_NOT_FOUND;
        if (is_link_or_reparse(absolute))
            return VITA_ERROR_TRAVERSAL;
        result = canonical_parent(absolute, canonical, sizeof(canonical));
        if (result != VITA_OK ||
            !path_is_beneath(canonical, mount->canonical_root))
            return VITA_ERROR_TRAVERSAL;
    }
    size_t n = strlen(absolute);
    if (n >= capacity) return VITA_ERROR_LIMIT;
    memcpy(out, absolute, n + 1);
    return VITA_OK;
}

static mount_entry *find_mount_for_path(const parsed_path *path,
                                        uint32_t flags, char *resolved,
                                        size_t capacity, int require_exists,
                                        int *error) {
    mount_entry *fallback = NULL;
    int best_error = VITA_ERROR_NOT_FOUND;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_entry *mount = &g_mounts[i];
        if (!mount->used || !prefix_equal(path->prefix, mount->prefix))
            continue;
        if (!fallback) fallback = mount;
        if (mount->kind == MOUNT_PSARC) {
            if (flags & (VITA_VFS_OPEN_WRITE | VITA_VFS_OPEN_CREATE |
                         VITA_VFS_OPEN_TRUNCATE | VITA_VFS_OPEN_APPEND)) {
                best_error = VITA_ERROR_READ_ONLY;
                continue;
            }
            if (psarc_find(mount->archive, path->relative) >= 0 ||
                psarc_has_dir(mount->archive, path->relative)) {
                if (error) *error = VITA_OK;
                return mount;
            }
            continue;
        }
        int result = resolve_mount(mount, path->relative, flags, resolved,
                                   capacity, require_exists);
        if (result == VITA_OK) {
            if (error) *error = VITA_OK;
            return mount;
        }
        if (result != VITA_ERROR_NOT_FOUND) best_error = result;
    }
    if (error) *error = best_error;
    (void)fallback;
    return NULL;
}

int vita_vfs_mount(const char *prefix, const char *host_root, uint32_t flags) {
    if (!host_root || !*host_root ||
        (flags & ~VITA_VFS_MOUNT_READ_ONLY))
        return VITA_ERROR_INVALID_ARGUMENT;
    char normalized_prefix[16];
    int result = normalize_prefix(prefix, normalized_prefix);
    if (result != VITA_OK) return result;
    if (prefix_equal(normalized_prefix, "app0"))
        flags |= VITA_VFS_MOUNT_READ_ONLY;

    char root[PATH_MAX], canonical[PATH_MAX];
    result = absolute_path(host_root, root, sizeof(root));
    if (result != VITA_OK) return result;
    result = canonical_existing(root, canonical, sizeof(canonical));
    if (result != VITA_OK) return result;
    host_stat_t stat_value;
    if (host_stat(root, &stat_value) != 0 ||
        !(stat_value.st_mode & S_IFDIR))
        return VITA_ERROR_INVALID_ARGUMENT;

    call_once(&g_once, vfs_once);
    mtx_lock(&g_lock);
    int index;
    for (index = 0; index < MAX_MOUNTS && g_mounts[index].used; index++) {}
    if (index == MAX_MOUNTS) {
        mtx_unlock(&g_lock);
        return VITA_ERROR_LIMIT;
    }
    mount_entry *mount = &g_mounts[index];
    memset(mount, 0, sizeof(*mount));
    mount->used = 1;
    mount->kind = MOUNT_DIRECTORY;
    mount->flags = flags;
    snprintf(mount->prefix, sizeof(mount->prefix), "%s", normalized_prefix);
    snprintf(mount->root, sizeof(mount->root), "%s", root);
    snprintf(mount->canonical_root, sizeof(mount->canonical_root), "%s",
             canonical);
    mtx_unlock(&g_lock);
    return VITA_OK;
}

int vita_vfs_mount_psarc(const char *prefix, const char *archive_path) {
    if (!archive_path || !*archive_path) return VITA_ERROR_INVALID_ARGUMENT;
    char normalized_prefix[16];
    int result = normalize_prefix(prefix, normalized_prefix);
    if (result != VITA_OK) return result;
    char why[160];
    psarc *archive = psarc_open(archive_path, why, sizeof(why));
    if (!archive) return VITA_ERROR_IO;

    call_once(&g_once, vfs_once);
    mtx_lock(&g_lock);
    int index;
    for (index = 0; index < MAX_MOUNTS && g_mounts[index].used; index++) {}
    if (index == MAX_MOUNTS) {
        mtx_unlock(&g_lock);
        psarc_close(archive);
        return VITA_ERROR_LIMIT;
    }
    mount_entry *mount = &g_mounts[index];
    memset(mount, 0, sizeof(*mount));
    mount->used = 1;
    mount->kind = MOUNT_PSARC;
    mount->flags = VITA_VFS_MOUNT_READ_ONLY;
    mount->archive = archive;
    snprintf(mount->prefix, sizeof(mount->prefix), "%s", normalized_prefix);
    snprintf(mount->root, sizeof(mount->root), "%s", archive_path);
    mtx_unlock(&g_lock);
    return VITA_OK;
}

int vita_vfs_resolve(const char *guest_path, uint32_t open_flags,
                     char *host_path, size_t host_path_capacity) {
    const uint32_t valid_flags =
        VITA_VFS_OPEN_READ | VITA_VFS_OPEN_WRITE |
        VITA_VFS_OPEN_CREATE | VITA_VFS_OPEN_TRUNCATE |
        VITA_VFS_OPEN_APPEND;
    if (!host_path || !host_path_capacity ||
        (open_flags & ~valid_flags) ||
        ((open_flags & (VITA_VFS_OPEN_CREATE | VITA_VFS_OPEN_TRUNCATE |
                        VITA_VFS_OPEN_APPEND)) &&
         !(open_flags & VITA_VFS_OPEN_WRITE)) ||
        ((open_flags & VITA_VFS_OPEN_APPEND) &&
         (open_flags & VITA_VFS_OPEN_TRUNCATE)))
        return VITA_ERROR_INVALID_ARGUMENT;
    host_path[0] = '\0';
    parsed_path path;
    int result = parse_guest_path(guest_path, &path);
    if (result != VITA_OK) return result;
    call_once(&g_once, vfs_once);
    mtx_lock(&g_lock);
    mount_entry *mount = find_mount_for_path(
        &path, open_flags, host_path, host_path_capacity,
        !(open_flags & VITA_VFS_OPEN_CREATE), &result);
    if (mount && mount->kind == MOUNT_PSARC)
        result = (open_flags & (VITA_VFS_OPEN_WRITE | VITA_VFS_OPEN_CREATE |
                                VITA_VFS_OPEN_TRUNCATE |
                                VITA_VFS_OPEN_APPEND))
               ? VITA_ERROR_READ_ONLY : VITA_ERROR_NOT_FOUND;
    mtx_unlock(&g_lock);
    return mount && result == VITA_OK ? VITA_OK : result;
}

static const char *mode_for_flags(uint32_t flags) {
    const int read = (flags & VITA_VFS_OPEN_READ) != 0;
    const int write = (flags & VITA_VFS_OPEN_WRITE) != 0;
    if (flags & VITA_VFS_OPEN_APPEND) return read ? "a+b" : "ab";
    if (flags & VITA_VFS_OPEN_TRUNCATE) return read ? "w+b" : "wb";
    if ((flags & VITA_VFS_OPEN_CREATE) && write) return read ? "r+b" : "r+b";
    if (write) return read ? "r+b" : "r+b";
    return "rb";
}

static FILE *open_host_file(const char *path, uint32_t flags) {
    FILE *file = fopen(path, mode_for_flags(flags));
    if (!file && (flags & VITA_VFS_OPEN_CREATE) &&
        (flags & VITA_VFS_OPEN_WRITE) &&
        !(flags & (VITA_VFS_OPEN_APPEND | VITA_VFS_OPEN_TRUNCATE)))
        file = fopen(path, (flags & VITA_VFS_OPEN_READ) ? "w+b" : "wb");
    return file;
}

int vita_vfs_open(const char *guest_path, uint32_t flags, vita_vfs_file **out) {
    const uint32_t valid_flags =
        VITA_VFS_OPEN_READ | VITA_VFS_OPEN_WRITE |
        VITA_VFS_OPEN_CREATE | VITA_VFS_OPEN_TRUNCATE |
        VITA_VFS_OPEN_APPEND;
    if (!out || (flags & ~valid_flags) ||
        !(flags & (VITA_VFS_OPEN_READ | VITA_VFS_OPEN_WRITE)))
        return VITA_ERROR_INVALID_ARGUMENT;
    if ((flags & (VITA_VFS_OPEN_CREATE | VITA_VFS_OPEN_TRUNCATE |
                  VITA_VFS_OPEN_APPEND)) &&
        !(flags & VITA_VFS_OPEN_WRITE))
        return VITA_ERROR_INVALID_ARGUMENT;
    if ((flags & VITA_VFS_OPEN_APPEND) &&
        (flags & VITA_VFS_OPEN_TRUNCATE))
        return VITA_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    parsed_path path;
    int result = parse_guest_path(guest_path, &path);
    if (result != VITA_OK) return result;
    call_once(&g_once, vfs_once);

    char resolved[PATH_MAX];
    mtx_lock(&g_lock);
    mount_entry *mount = find_mount_for_path(
        &path, flags, resolved, sizeof(resolved),
        !(flags & VITA_VFS_OPEN_CREATE), &result);
    if (!mount || result != VITA_OK) {
        mtx_unlock(&g_lock);
        return result;
    }
    vita_vfs_file *file = (vita_vfs_file *)calloc(1, sizeof(*file));
    if (!file || mtx_init(&file->lock, mtx_plain) != thrd_success) {
        free(file);
        mtx_unlock(&g_lock);
        return VITA_ERROR_NO_MEMORY;
    }
    file->flags = flags;
    file->mount_flags = mount->flags;
    if (mount->kind == MOUNT_PSARC) {
        int index = psarc_find(mount->archive, path.relative);
        file->archive = psarc_file_open(mount->archive, index);
        if (!file->archive) result = VITA_ERROR_NOT_FOUND;
    } else {
        file->host = open_host_file(resolved, flags);
        if (!file->host) result = VITA_ERROR_IO;
    }
    if (result != VITA_OK) {
        mtx_destroy(&file->lock);
        free(file);
        mtx_unlock(&g_lock);
        return result;
    }
    file->next = g_files;
    g_files = file;
    mtx_unlock(&g_lock);
    vita_runtime_count(VITA_COUNTER_FILE_OPENED, 1);
    *out = file;
    return VITA_OK;
}

int64_t vita_vfs_read(vita_vfs_file *file, void *dst, size_t size) {
    if (!file || (!dst && size)) return VITA_ERROR_INVALID_ARGUMENT;
    if (!(file->flags & VITA_VFS_OPEN_READ))
        return VITA_ERROR_INVALID_ARGUMENT;
    if (size > INT64_MAX) return VITA_ERROR_OVERFLOW;
    if (!size) return 0;
    mtx_lock(&file->lock);
    int64_t result;
    if (file->archive) {
        result = psarc_file_read(file->archive, file->position, dst, size);
        if (result > 0) file->position += (uint64_t)result;
    } else {
        size_t read = fread(dst, 1, size, file->host);
        result = read || feof(file->host) ? (int64_t)read : VITA_ERROR_IO;
    }
    mtx_unlock(&file->lock);
    return result;
}

int64_t vita_vfs_write(vita_vfs_file *file, const void *src, size_t size) {
    if (!file || (!src && size)) return VITA_ERROR_INVALID_ARGUMENT;
    if (file->archive || (file->mount_flags & VITA_VFS_MOUNT_READ_ONLY))
        return VITA_ERROR_READ_ONLY;
    if (!(file->flags & VITA_VFS_OPEN_WRITE))
        return VITA_ERROR_INVALID_ARGUMENT;
    if (!size) return 0;
    mtx_lock(&file->lock);
    if (size > INT64_MAX) {
        mtx_unlock(&file->lock);
        return VITA_ERROR_OVERFLOW;
    }
    size_t written = fwrite(src, 1, size, file->host);
    int failed = written != size && ferror(file->host);
    mtx_unlock(&file->lock);
    return failed ? VITA_ERROR_IO : (int64_t)written;
}

int vita_vfs_seek(vita_vfs_file *file, int64_t offset, int whence,
                  uint64_t *position) {
    if (!file || whence < SEEK_SET || whence > SEEK_END)
        return VITA_ERROR_INVALID_ARGUMENT;
    mtx_lock(&file->lock);
    int result = VITA_OK;
    if (file->archive) {
        uint64_t size = psarc_file_size(file->archive);
        int64_t base = 0;
        if (whence == SEEK_CUR) {
            if (file->position > INT64_MAX) result = VITA_ERROR_OVERFLOW;
            else base = (int64_t)file->position;
        } else if (whence == SEEK_END) {
            if (size > INT64_MAX) result = VITA_ERROR_OVERFLOW;
            else base = (int64_t)size;
        }
        if (result == VITA_OK) {
            uint64_t magnitude = offset < 0
                ? (uint64_t)(-(offset + 1)) + 1u : 0;
            if ((offset > 0 && base > INT64_MAX - offset) ||
                (offset < 0 && magnitude > (uint64_t)base)) {
                result = VITA_ERROR_OVERFLOW;
            } else {
                int64_t target = base + offset;
                if (target < 0) result = VITA_ERROR_INVALID_ARGUMENT;
                else {
                    file->position = (uint64_t)target;
                    if (position) *position = file->position;
                }
            }
        }
    } else if (host_seek(file->host, offset, whence) != 0) {
        result = VITA_ERROR_IO;
    } else if (position) {
        int64_t current = host_tell(file->host);
        if (current < 0) result = VITA_ERROR_IO;
        else *position = (uint64_t)current;
    }
    mtx_unlock(&file->lock);
    return result;
}

int vita_vfs_file_stat(vita_vfs_file *file, vita_vfs_stat *out) {
    if (!file || !out) return VITA_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    mtx_lock(&file->lock);
    out->type = VITA_VFS_NODE_FILE;
    out->mount_flags = file->mount_flags;
    if (file->archive) {
        out->size = psarc_file_size(file->archive);
    } else {
        int64_t current = host_tell(file->host);
        if (current < 0 || host_seek(file->host, 0, SEEK_END) != 0) {
            mtx_unlock(&file->lock);
            return VITA_ERROR_IO;
        }
        int64_t end = host_tell(file->host);
        (void)host_seek(file->host, current, SEEK_SET);
        if (end < 0) {
            mtx_unlock(&file->lock);
            return VITA_ERROR_IO;
        }
        out->size = (uint64_t)end;
    }
    mtx_unlock(&file->lock);
    return VITA_OK;
}

void vita_vfs_close(vita_vfs_file *file) {
    if (!file) return;
    call_once(&g_once, vfs_once);
    mtx_lock(&g_lock);
    vita_vfs_file **link = &g_files;
    while (*link && *link != file) link = &(*link)->next;
    if (*link) *link = file->next;
    mtx_unlock(&g_lock);
    if (file->host) fclose(file->host);
    psarc_file_close(file->archive);
    mtx_destroy(&file->lock);
    free(file);
    vita_runtime_count(VITA_COUNTER_FILE_CLOSED, 1);
}

static void stat_from_host(const host_stat_t *value, uint32_t flags,
                           vita_vfs_stat *out) {
    memset(out, 0, sizeof(*out));
    out->type = (value->st_mode & S_IFDIR)
              ? VITA_VFS_NODE_DIRECTORY : VITA_VFS_NODE_FILE;
    out->size = value->st_size < 0 ? 0 : (uint64_t)value->st_size;
    out->modified_unix_us =
        value->st_mtime < 0 ? 0 : (uint64_t)value->st_mtime * UINT64_C(1000000);
    out->mount_flags = flags;
}

int vita_vfs_stat_path(const char *guest_path, vita_vfs_stat *out) {
    if (!out) return VITA_ERROR_INVALID_ARGUMENT;
    parsed_path path;
    int result = parse_guest_path(guest_path, &path);
    if (result != VITA_OK) return result;
    char resolved[PATH_MAX];
    call_once(&g_once, vfs_once);
    mtx_lock(&g_lock);
    mount_entry *mount = find_mount_for_path(
        &path, VITA_VFS_OPEN_READ, resolved, sizeof(resolved), 1, &result);
    if (!mount || result != VITA_OK) {
        mtx_unlock(&g_lock);
        return result;
    }
    if (mount->kind == MOUNT_PSARC) {
        int index = psarc_find(mount->archive, path.relative);
        memset(out, 0, sizeof(*out));
        if (index >= 0) {
            out->type = VITA_VFS_NODE_FILE;
            out->size = psarc_size(mount->archive, index);
        } else if (psarc_has_dir(mount->archive, path.relative)) {
            out->type = VITA_VFS_NODE_DIRECTORY;
        } else {
            mtx_unlock(&g_lock);
            return VITA_ERROR_NOT_FOUND;
        }
        out->mount_flags = mount->flags;
    } else {
        host_stat_t value;
        if (host_stat(resolved, &value) != 0) {
            mtx_unlock(&g_lock);
            return VITA_ERROR_NOT_FOUND;
        }
        stat_from_host(&value, mount->flags, out);
    }
    mtx_unlock(&g_lock);
    return VITA_OK;
}

int vita_vfs_remove(const char *guest_path) {
    char resolved[PATH_MAX];
    int result = vita_vfs_resolve(
        guest_path, VITA_VFS_OPEN_WRITE, resolved, sizeof(resolved));
    if (result != VITA_OK) return result;
    return remove(resolved) == 0 ? VITA_OK : VITA_ERROR_IO;
}

int vita_vfs_rename(const char *from, const char *to) {
    char source[PATH_MAX], destination[PATH_MAX];
    int result = vita_vfs_resolve(
        from, VITA_VFS_OPEN_WRITE, source, sizeof(source));
    if (result != VITA_OK) return result;
    result = vita_vfs_resolve(
        to, VITA_VFS_OPEN_WRITE | VITA_VFS_OPEN_CREATE,
        destination, sizeof(destination));
    if (result != VITA_OK) return result;
    return rename(source, destination) == 0 ? VITA_OK : VITA_ERROR_IO;
}

int vita_vfs_opendir(const char *guest_path, vita_vfs_dir **out) {
    if (!out) return VITA_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    parsed_path path;
    int result = parse_guest_path(guest_path, &path);
    if (result != VITA_OK) return result;
    char resolved[PATH_MAX] = {0};
    vita_vfs_stat value;
    result = vita_vfs_stat_path(guest_path, &value);
    if (result != VITA_OK || value.type != VITA_VFS_NODE_DIRECTORY)
        return VITA_ERROR_NOT_FOUND;

    vita_vfs_dir *dir = (vita_vfs_dir *)calloc(1, sizeof(*dir));
    if (!dir || mtx_init(&dir->lock, mtx_plain) != thrd_success) {
        free(dir);
        return VITA_ERROR_NO_MEMORY;
    }
    dir->mount_flags = value.mount_flags;
    call_once(&g_once, vfs_once);
    mtx_lock(&g_lock);
    mount_entry *mount = find_mount_for_path(
        &path, VITA_VFS_OPEN_READ, resolved, sizeof(resolved), 1, &result);
    if (!mount || result != VITA_OK) {
        mtx_unlock(&g_lock);
        mtx_destroy(&dir->lock);
        free(dir);
        return result;
    }
    if (mount->kind == MOUNT_PSARC) {
        dir->archive = mount->archive;
        snprintf(dir->archive_path, sizeof(dir->archive_path), "%s",
                 path.relative);
    } else {
        snprintf(dir->root, sizeof(dir->root), "%s", resolved);
    }
    mtx_unlock(&g_lock);
    if (!dir->archive) {
#ifdef _WIN32
    char pattern[PATH_MAX];
    int written = snprintf(pattern, sizeof(pattern), "%s\\*", resolved);
    if (written < 0 || (size_t)written >= sizeof(pattern)) {
        mtx_destroy(&dir->lock);
        free(dir);
        return VITA_ERROR_LIMIT;
    }
    dir->search = _findfirst(pattern, &dir->current);
    if (dir->search == -1) {
        mtx_destroy(&dir->lock);
        free(dir);
        return VITA_ERROR_IO;
    }
    dir->have_current = 1;
#else
    dir->host = opendir(resolved);
    if (!dir->host) {
        mtx_destroy(&dir->lock);
        free(dir);
        return VITA_ERROR_IO;
    }
#endif
    }
    mtx_lock(&g_lock);
    dir->next = g_dirs;
    g_dirs = dir;
    mtx_unlock(&g_lock);
    *out = dir;
    return VITA_OK;
}

int vita_vfs_readdir(vita_vfs_dir *dir, char *name, size_t capacity,
                     vita_vfs_stat *stat_out) {
    if (!dir || !name || !capacity) return VITA_ERROR_INVALID_ARGUMENT;
    mtx_lock(&dir->lock);
    const char *entry = NULL;
    char archive_name[1024];
    if (dir->archive) {
        int is_directory = 0;
        uint64_t size = 0;
        int archive_result = psarc_readdir(
            dir->archive, dir->archive_path, &dir->archive_cursor,
            archive_name, sizeof(archive_name), &is_directory, &size);
        if (archive_result <= 0) {
            mtx_unlock(&dir->lock);
            return archive_result == 0 ? VITA_ERROR_NOT_FOUND
                                       : VITA_ERROR_IO;
        }
        entry = archive_name;
        if (stat_out) {
            memset(stat_out, 0, sizeof(*stat_out));
            stat_out->type = is_directory ? VITA_VFS_NODE_DIRECTORY
                                          : VITA_VFS_NODE_FILE;
            stat_out->size = size;
            stat_out->mount_flags = dir->mount_flags;
        }
    } else {
#ifdef _WIN32
    for (;;) {
        if (!dir->have_current) {
            if (_findnext(dir->search, &dir->current) != 0) break;
        }
        dir->have_current = 0;
        if (strcmp(dir->current.name, ".") && strcmp(dir->current.name, "..")) {
            entry = dir->current.name;
            break;
        }
    }
#else
    struct dirent *value;
    while ((value = readdir(dir->host)) != NULL) {
        if (strcmp(value->d_name, ".") && strcmp(value->d_name, "..")) {
            entry = value->d_name;
            break;
        }
    }
#endif
    }
    if (!entry) {
        mtx_unlock(&dir->lock);
        return VITA_ERROR_NOT_FOUND;
    }
    size_t n = strlen(entry);
    if (n >= capacity) {
        mtx_unlock(&dir->lock);
        return VITA_ERROR_LIMIT;
    }
    memcpy(name, entry, n + 1);
    if (stat_out && !dir->archive) {
        char child[PATH_MAX];
        int written = snprintf(child, sizeof(child), "%s%c%s",
                               dir->root, PATH_SEP, entry);
        host_stat_t value;
        if (written < 0 || (size_t)written >= sizeof(child) ||
            host_stat(child, &value) != 0) {
            mtx_unlock(&dir->lock);
            return VITA_ERROR_IO;
        }
        stat_from_host(&value, dir->mount_flags, stat_out);
    }
    mtx_unlock(&dir->lock);
    return VITA_OK;
}

void vita_vfs_closedir(vita_vfs_dir *dir) {
    if (!dir) return;
    call_once(&g_once, vfs_once);
    mtx_lock(&g_lock);
    vita_vfs_dir **link = &g_dirs;
    while (*link && *link != dir) link = &(*link)->next;
    if (*link) *link = dir->next;
    mtx_unlock(&g_lock);
#ifdef _WIN32
    if (!dir->archive && dir->search != -1) _findclose(dir->search);
#else
    if (!dir->archive && dir->host) closedir(dir->host);
#endif
    mtx_destroy(&dir->lock);
    free(dir);
}

void vita_vfs_unmount_all(void) {
    call_once(&g_once, vfs_once);
    for (;;) {
        mtx_lock(&g_lock);
        vita_vfs_file *file = g_files;
        mtx_unlock(&g_lock);
        if (!file) break;
        vita_vfs_close(file);
    }
    for (;;) {
        mtx_lock(&g_lock);
        vita_vfs_dir *dir = g_dirs;
        mtx_unlock(&g_lock);
        if (!dir) break;
        vita_vfs_closedir(dir);
    }
    mtx_lock(&g_lock);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].archive) psarc_close(g_mounts[i].archive);
        memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
    }
    mtx_unlock(&g_lock);
}
