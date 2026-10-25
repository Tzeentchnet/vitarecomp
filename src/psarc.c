/* psarc.c — a PlayStation archive, read in place (psarc.h).
 *
 * The layout, every field big-endian:
 *
 *   0x00  "PSAR"
 *   0x04  version: major, minor (16 bits each); 1.4 is the one in use
 *   0x08  the compression, four letters: "zlib"
 *   0x0C  the length of the table of contents, this header included
 *   0x10  the size of one entry (30)
 *   0x14  the number of entries
 *   0x18  the block size (65536)
 *   0x1C  flags: 1 = names compare without case, 2 = names are absolute
 *   0x20  the entries: a 16-byte digest of the name, the index of the file's
 *         first block in the size table (32 bits), the file's length and
 *         its offset in the archive (40 bits each)
 *   then  the size table: for every block of every file, how many bytes it
 *         takes in the archive, in as many bytes as hold the block size
 *         (two for 65536). Zero says the block is a whole one, stored.
 *
 * Entry 0 is the manifest: the names of entries 1.., one to a line. A block
 * that holds as many bytes as it stands for is stored; any other is a zlib
 * stream. The digests are not used: the manifest gives every name, and a
 * sorted copy of it finds one.
 */

#include "psarc.h"

#include "inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#ifdef _WIN32
#define arc_seek(f, off) _fseeki64(f, (long long)(off), SEEK_SET)
#define arc_seek_end(f) _fseeki64(f, 0, SEEK_END)
#define arc_tell(f) _ftelli64(f)
#else
#define arc_seek(f, off) fseeko(f, (off_t)(off), SEEK_SET)
#define arc_seek_end(f) fseeko(f, 0, SEEK_END)
#define arc_tell(f) ftello(f)
#endif

#define PSARC_MAX_TOC      (64u << 20)
#define PSARC_MAX_ENTRIES  1000000u
#define PSARC_MAX_BLOCK    (16u << 20)
#define PSARC_MAX_MANIFEST (64u << 20)
#define PSARC_MAX_NAME     1023u

typedef struct {
    uint32_t first;     /* its first block's index in the size table */
    uint64_t size;
    uint64_t offset;
} entry;

typedef struct {
    const char *key;    /* the name, folded */
    uint32_t    index;  /* the entry */
} name_ref;

struct psarc {
    FILE     *f;
    mtx_t     lock;
    int       lock_init;
    uint64_t  archive_size;
    uint32_t  block;
    uint32_t  entries;
    entry    *e;
    uint32_t  sizes_n;
    uint32_t *sizes;
    char     *manifest;     /* the names as written, NUL-separated */
    char    **names;        /* entry i's name is names[i - 1] */
    char     *folded;
    name_ref *sorted;
    uint32_t  files;
    uint8_t  *packed;       /* one block as the archive has it */
};

struct psarc_file {
    psarc   *a;
    entry    e;
    uint32_t at_block;      /* the block the cursor is at */
    uint64_t at_offset;     /* and where it begins in the archive */
    uint8_t *data;          /* the block in `have`, unpacked */
    uint32_t have;          /* its index, or ~0 for none */
    uint32_t have_len;
};

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be40(const uint8_t *p) { return ((uint64_t)p[0] << 32) | be32(p + 1); }

static char fold(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c == '\\' ? '/' : c;
}

/* A name without its leading slashes, folded into `out`. */
static int fold_name(const char *name, char *out, size_t cap) {
    size_t n = 0;
    while (*name == '/' || *name == '\\') name++;
    for (; *name; name++) {
        char c = *name == '\\' ? '/' : fold(*name);
        if (c == '/' && (!n || out[n - 1] == '/')) continue;
        if (n + 1 >= cap) return 0;
        out[n++] = c;
    }
    while (n && out[n - 1] == '/') n--;
    out[n] = '\0';
    return 1;
}

static int by_key(const void *x, const void *y) { return strcmp(((const name_ref *)x)->key, ((const name_ref *)y)->key); }

/* How many bytes block `k` of a file takes in the archive, and how many it
 * stands for. */
static uint32_t block_plain(const psarc *a, const entry *e, uint32_t k) {
    const uint64_t left = e->size - (uint64_t)k * a->block;
    return left < a->block ? (uint32_t)left : a->block;
}
static uint32_t block_packed(const psarc *a, const entry *e, uint32_t k) {
    const uint32_t z = e->first + k < a->sizes_n ? a->sizes[e->first + k] : 0;
    return z ? z : block_plain(a, e, k);
}

/* A whole file into `dst`, which holds its size. 0 when it was read. */
static int read_whole(psarc *a, const entry *e, uint8_t *dst) {
    uint64_t done = 0;
    if (arc_seek(a->f, e->offset)) return -1;
    for (uint32_t k = 0; done < e->size; k++) {
        const uint32_t plain = block_plain(a, e, k), packed = block_packed(a, e, k);
        if (packed > a->block) return -1;
        if (packed == plain) {
            if (fread(dst + (size_t)done, 1, plain, a->f) != plain) return -1;
        } else {
            size_t out = plain;
            if (fread(a->packed, 1, packed, a->f) != packed) return -1;
            if (inf_zlib(a->packed, packed, dst + (size_t)done, &out) != INF_OK || out != plain) return -1;
        }
        done += plain;
    }
    return 0;
}

psarc *psarc_open(const char *host_path, char *why, size_t why_cap) {
    uint8_t head[0x20];
    uint8_t *toc = NULL;
    psarc *a = (psarc *)calloc(1, sizeof *a);
    if (why && why_cap) why[0] = '\0';
    if (!a) return NULL;
#define FAIL(text) do { \
    if (why && why_cap) snprintf(why, why_cap, "%s", text); \
    psarc_close(a); free(toc); return NULL; \
} while (0)
    if (mtx_init(&a->lock, mtx_plain) != thrd_success) FAIL("there is no lock for the archive");
    a->lock_init = 1;
    a->f = fopen(host_path, "rb");
    if (!a->f) FAIL("the file cannot be opened");
    if (arc_seek(a->f, 0) != 0 || arc_seek_end(a->f) != 0)
        FAIL("its length cannot be read");
    int64_t archive_size = arc_tell(a->f);
    if (archive_size < 0x20 || arc_seek(a->f, 0) != 0)
        FAIL("it is too short");
    a->archive_size = (uint64_t)archive_size;
    if (fread(head, 1, sizeof head, a->f) != sizeof head || memcmp(head, "PSAR", 4)) FAIL("it does not begin with PSAR");
    if (memcmp(head + 8, "zlib", 4)) FAIL("its blocks are packed some way other than zlib");
    const uint32_t toc_len = be32(head + 0x0C), entry_size = be32(head + 0x10);
    a->entries = be32(head + 0x14);
    a->block = be32(head + 0x18);
    if (entry_size != 30 || a->entries < 1 ||
        a->entries > PSARC_MAX_ENTRIES ||
        a->block < 0x100 || a->block > PSARC_MAX_BLOCK ||
        toc_len < 0x20 || toc_len > PSARC_MAX_TOC ||
        toc_len > a->archive_size ||
        (uint64_t)a->entries * 30 > toc_len - 0x20)
        FAIL("its table of contents makes no sense");
    toc = (uint8_t *)malloc(toc_len - 0x20);
    if (!toc || fread(toc, 1, toc_len - 0x20, a->f) != toc_len - 0x20) FAIL("its table of contents is cut short");

    uint32_t width = 1;
    while (width < 4 && ((uint64_t)1 << (8 * width)) < a->block) width++;
    a->e = (entry *)calloc(a->entries, sizeof *a->e);
    uint32_t sizes_bytes = toc_len - 0x20 - a->entries * 30;
    if (sizes_bytes % width) FAIL("its block size table is misaligned");
    a->sizes_n = sizes_bytes / width;
    a->sizes = (uint32_t *)calloc(a->sizes_n ? a->sizes_n : 1, sizeof *a->sizes);
    a->packed = (uint8_t *)malloc(a->block);
    if (!a->e || !a->sizes || !a->packed) FAIL("there is no memory for its table of contents");
    for (uint32_t i = 0; i < a->entries; i++) {
        const uint8_t *p = toc + (size_t)i * 30;
        a->e[i].first = be32(p + 16);
        a->e[i].size = be40(p + 20);
        a->e[i].offset = be40(p + 25);
    }
    const uint8_t *t = toc + (size_t)a->entries * 30;
    for (uint32_t i = 0; i < a->sizes_n; i++, t += width) {
        uint32_t v = 0;
        for (uint32_t b = 0; b < width; b++) v = (v << 8) | t[b];
        a->sizes[i] = v;
        if (v > a->block) FAIL("a packed block is larger than a plain block");
    }
    free(toc);
    toc = NULL;

    /* Prove every entry's block span and packed data range before any entry
     * can be opened. */
    for (uint32_t i = 0; i < a->entries; i++) {
        entry *value = &a->e[i];
        uint64_t blocks64 = value->size
            ? (value->size - 1) / a->block + 1 : 0;
        if (blocks64 > UINT32_MAX ||
            value->first > a->sizes_n ||
            blocks64 > a->sizes_n - value->first ||
            (value->size && value->offset < toc_len) ||
            value->offset > a->archive_size)
            FAIL("an entry points outside the archive");
        uint64_t packed_total = 0;
        for (uint32_t k = 0; k < (uint32_t)blocks64; k++) {
            uint32_t packed = block_packed(a, value, k);
            if (!packed || packed > a->block ||
                packed_total > UINT64_MAX - packed)
                FAIL("an entry has an invalid block span");
            packed_total += packed;
        }
        if (packed_total > a->archive_size - value->offset)
            FAIL("an entry is cut short");
    }

    /* the manifest: the names of every entry after it, a line each */
    const uint64_t msize = a->e[0].size;
    if (msize > PSARC_MAX_MANIFEST || msize > SIZE_MAX - 1)
        FAIL("its manifest is far too long");
    a->manifest = (char *)malloc((size_t)msize + 1);
    a->folded = (char *)malloc((size_t)msize + 1);
    a->files = a->entries - 1;
    a->names = (char **)calloc(a->files ? a->files : 1, sizeof *a->names);
    a->sorted = (name_ref *)calloc(a->files ? a->files : 1, sizeof *a->sorted);
    if (!a->manifest || !a->folded || !a->names || !a->sorted) FAIL("there is no memory for its names");
    if (read_whole(a, &a->e[0], (uint8_t *)a->manifest)) FAIL("its manifest cannot be read");
    a->manifest[msize] = '\0';
    uint32_t n = 0;
    char *cursor = a->manifest;
    char *limit = a->manifest + (size_t)msize;
    while (n < a->files && cursor < limit) {
        char *end = (char *)memchr(cursor, '\n', (size_t)(limit - cursor));
        if (!end) end = limit;
        if (end == cursor || (size_t)(end - cursor) > PSARC_MAX_NAME)
            FAIL("its manifest has an invalid name");
        if (memchr(cursor, '\0', (size_t)(end - cursor)))
            FAIL("its manifest contains an embedded terminator");
        *end = '\0';
        /* Archive names are lookup keys, never extraction paths, but rejecting
         * traversal components keeps mounted namespace semantics unambiguous. */
        const char *component = cursor;
        for (const char *p = cursor;; p++) {
            if (*p == '/' || *p == '\\' || *p == '\0') {
                if (p - component == 2 && component[0] == '.' &&
                    component[1] == '.')
                    FAIL("its manifest contains traversal");
                if (!*p) break;
                component = p + 1;
            }
        }
        a->names[n++] = cursor;
        cursor = end < limit ? end + 1 : limit;
    }
    while (cursor < limit && (*cursor == '\n' || *cursor == '\r')) cursor++;
    if (n != a->files || cursor != limit)
        FAIL("its manifest does not name exactly its files");
    for (uint32_t i = 0; i < a->files; i++) {
        char *key = a->folded + (a->names[i] - a->manifest);
        if (!fold_name(a->names[i], key, strlen(a->names[i]) + 1))
            FAIL("its manifest has a name that is too long");
        a->sorted[i].key = key;
        a->sorted[i].index = i + 1;
    }
    qsort(a->sorted, a->files, sizeof *a->sorted, by_key);
    for (uint32_t i = 1; i < a->files; i++)
        if (strcmp(a->sorted[i - 1].key, a->sorted[i].key) == 0)
            FAIL("its manifest contains duplicate names");
#undef FAIL
    return a;
}

void psarc_close(psarc *a) {
    if (!a) return;
    if (a->f) fclose(a->f);
    free(a->e);
    free(a->sizes);
    free(a->manifest);
    free(a->names);
    free(a->folded);
    free(a->sorted);
    free(a->packed);
    if (a->lock_init) mtx_destroy(&a->lock);
    free(a);
}

uint32_t psarc_count(const psarc *a) { return a ? a->files : 0; }
const char *psarc_name(const psarc *a, uint32_t index) {
    return a && index < a->files ? a->names[index] : NULL;
}

int psarc_find(const psarc *a, const char *name) {
    char key[1024];
    if (!a || !name || !fold_name(name, key, sizeof key)) return -1;
    uint32_t lo = 0, hi = a->files;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const int c = strcmp(a->sorted[mid].key, key);
        if (!c) return (int)a->sorted[mid].index;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

uint64_t psarc_size(const psarc *a, int index) {
    return a && index > 0 && (uint32_t)index < a->entries
         ? a->e[index].size : 0;
}

int psarc_has_dir(const psarc *a, const char *dir) {
    char key[1024];
    if (!a || !dir || !fold_name(dir, key, sizeof key - 1)) return 0;
    size_t n = strlen(key);
    while (n && key[n - 1] == '/') n--;
    if (!n) return a->files != 0;
    key[n++] = '/';
    key[n] = '\0';
    uint32_t lo = 0, hi = a->files;
    while (lo < hi) {   /* the first name not below the key */
        const uint32_t mid = lo + (hi - lo) / 2;
        if (strcmp(a->sorted[mid].key, key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo < a->files && !strncmp(a->sorted[lo].key, key, n);
}

int psarc_readdir(const psarc *a, const char *dir, uint32_t *cursor,
                  char *name, size_t capacity, int *is_directory,
                  uint64_t *size) {
    if (!a || !dir || !cursor || !name || !capacity) return -1;
    char prefix[1024];
    if (!fold_name(dir, prefix, sizeof(prefix) - 1)) return -1;
    size_t prefix_length = strlen(prefix);
    if (prefix_length) prefix[prefix_length++] = '/';
    prefix[prefix_length] = '\0';

    uint32_t i = *cursor;
    while (i < a->files) {
        const name_ref *ref = &a->sorted[i];
        if (strncmp(ref->key, prefix, prefix_length) != 0) {
            i++;
            continue;
        }
        const char *relative = ref->key + prefix_length;
        if (!*relative) {
            i++;
            continue;
        }
        const char *slash = strchr(relative, '/');
        size_t child_length = slash ? (size_t)(slash - relative)
                                    : strlen(relative);
        if (!child_length || child_length >= capacity) return -1;
        memcpy(name, relative, child_length);
        name[child_length] = '\0';
        int directory = slash != NULL;

        i++;
        while (i < a->files) {
            const char *next = a->sorted[i].key;
            if (strncmp(next, prefix, prefix_length) != 0) break;
            next += prefix_length;
            if (strncmp(next, name, child_length) != 0 ||
                (next[child_length] != '/' && next[child_length] != '\0'))
                break;
            i++;
        }
        *cursor = i;
        if (is_directory) *is_directory = directory;
        if (size) *size = directory ? 0 : a->e[ref->index].size;
        return 1;
    }
    *cursor = i;
    return 0;
}

psarc_file *psarc_file_open(psarc *a, int index) {
    if (!a || index <= 0 || (uint32_t)index >= a->entries) return NULL;
    psarc_file *f = (psarc_file *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->a = a;
    f->e = a->e[index];
    f->at_offset = f->e.offset;
    f->have = ~0u;
    f->data = (uint8_t *)malloc(a->block);
    if (!f->data) { free(f); return NULL; }
    return f;
}

void psarc_file_close(psarc_file *f) {
    if (!f) return;
    free(f->data);
    free(f);
}

uint64_t psarc_file_size(const psarc_file *f) { return f ? f->e.size : 0; }
int psarc_file_is_from(const psarc_file *file, const psarc *archive) {
    return file && file->a == archive;
}

/* The cursor to block `k`: forwards from where it is, or from the file's
 * first block when `k` is behind it. */
static void seek_block(psarc_file *f, uint32_t k) {
    if (k < f->at_block) { f->at_block = 0; f->at_offset = f->e.offset; }
    while (f->at_block < k) {
        f->at_offset += block_packed(f->a, &f->e, f->at_block);
        f->at_block++;
    }
}

/* Block `k` into `dst`, which holds a block. Its length, or -1. */
static int64_t fetch_block(psarc_file *f, uint32_t k, uint8_t *dst) {
    psarc *a = f->a;
    seek_block(f, k);
    const uint32_t plain = block_plain(a, &f->e, k), packed = block_packed(a, &f->e, k);
    if (packed > a->block || arc_seek(a->f, f->at_offset)) return -1;
    if (packed == plain) return fread(dst, 1, plain, a->f) == plain ? (int64_t)plain : -1;
    size_t out = plain;
    if (fread(a->packed, 1, packed, a->f) != packed) return -1;
    if (inf_zlib(a->packed, packed, dst, &out) != INF_OK || out != plain) return -1;
    return (int64_t)plain;
}

int64_t psarc_file_read(psarc_file *f, uint64_t offset, void *dst, uint64_t length) {
    if (!f || (!dst && length) || length > SIZE_MAX ||
        length > INT64_MAX)
        return -1;
    psarc *a = f->a;
    uint8_t *out = (uint8_t *)dst;
    uint64_t done = 0;
    if (offset >= f->e.size) return 0;
    if (length > f->e.size - offset) length = f->e.size - offset;
    mtx_lock(&a->lock);
    while (done < length) {
        const uint32_t k = (uint32_t)((offset + done) / a->block);
        const uint32_t in = (uint32_t)((offset + done) % a->block);
        const uint32_t plain = block_plain(a, &f->e, k);
        if (!in && length - done >= plain) {    /* a whole block: straight to the caller */
            if (fetch_block(f, k, out + (size_t)done) < 0) {
                mtx_unlock(&a->lock);
                return -1;
            }
            done += plain;
            continue;
        }
        if (f->have != k) {
            const int64_t got = fetch_block(f, k, f->data);
            if (got < 0) {
                f->have = ~0u;
                mtx_unlock(&a->lock);
                return -1;
            }
            f->have = k;
            f->have_len = (uint32_t)got;
        }
        uint32_t take = f->have_len - in;
        if (take > length - done) take = (uint32_t)(length - done);
        memcpy(out + (size_t)done, f->data + in, take);
        done += take;
    }
    mtx_unlock(&a->lock);
    return (int64_t)done;
}
