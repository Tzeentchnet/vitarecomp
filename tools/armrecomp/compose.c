/* compose.c -- strict, deterministic generated-module composition. */

#include "compose.h"
#include "emit.h"

#include "vitarecomp/generated_module.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AC_COMPOSE_SCHEMA_VERSION 4u
#define AC_MAX_JSON_BYTES (256u * 1024u * 1024u)
#define AC_MAX_JSON_DEPTH 64u

typedef struct {
    char *text;
    size_t size;
} ac_file;

typedef enum {
    JV_NULL,
    JV_BOOL,
    JV_UINT,
    JV_STRING,
    JV_ARRAY,
    JV_OBJECT
} jv_kind;

typedef struct jvalue jvalue;

typedef struct {
    char *name;
    jvalue *value;
} jmember;

struct jvalue {
    jv_kind kind;
    union {
        int boolean;
        uint64_t uint_value;
        char *string;
        struct {
            jvalue **items;
            size_t count;
        } array;
        struct {
            jmember *members;
            size_t count;
        } object;
    } as;
};

typedef struct {
    const char *cur;
    const char *end;
    const char *path;
    char *error;
    size_t error_size;
    int failed;
} jparser;

typedef struct {
    uint32_t begin;
    uint32_t end;
} ac_range;

typedef enum {
    AC_DISPATCH_FUNCTION,
    AC_DISPATCH_IMPORT
} ac_dispatch_kind;

typedef struct {
    uint32_t key;
    ac_dispatch_kind kind;
    char *symbol;
    uint32_t library_nid;
    uint32_t function_nid;
} ac_dispatch;

typedef struct {
    uint32_t key;
    uint32_t stub_address;
    uint32_t target_site;
    uint32_t library_nid;
    uint32_t function_nid;
    char *name;
    char *symbol;
    char *target_slot;
    int strict_unsupported;
} ac_function_import;

typedef enum {
    AC_EXPORT_FUNCTION,
    AC_EXPORT_VARIABLE,
    AC_EXPORT_TLS_VARIABLE
} ac_export_kind;

typedef struct {
    ac_export_kind kind;
    uint32_t library_nid;
    uint32_t nid;
    uint32_t address;
    uint32_t dispatch_key;
    int has_dispatch_key;
} ac_export;

typedef struct {
    uint32_t address;
    uint32_t addend;
    uint32_t relocation_code;
} ac_variable_site;

typedef struct {
    uint32_t library_nid;
    uint32_t variable_nid;
    uint32_t stub_address;
    char *name;
    char *symbol;
    ac_variable_site *sites;
    size_t site_count;
} ac_variable_import;

typedef struct {
    char *kind;
    uint64_t count;
} ac_trap_kind;

typedef struct {
    char *unit;
    uint32_t functions;
    uint32_t instructions;
    uint32_t first_key;
    uint32_t last_key;
} ac_partition;

typedef struct {
    char *module_id;
    char *module_name;
    int structural_ready;
    int production_ready;
    int instrumentation_function_entry;
    int instrumentation_pc;
    int entry_present;
    uint32_t entry_raw_address;
    uint32_t entry_dispatch_key;
    int entry_resolved;
    int tls_present;
    int tls_resolved;
    uint32_t tls_address;
    uint32_t tls_file_size;
    uint32_t tls_memory_size;
    uint32_t tls_alignment;
    uint32_t functions_emitted;
    uint32_t functions_discovered;
    uint32_t functions_dropped;
    uint32_t functions_promoted;
    uint32_t instructions_total;
    uint32_t instructions_translated;
    uint32_t instructions_trapped;
    uint32_t body_cuts;
    uint64_t traps_total;
    uint64_t traps_classified;
    uint32_t traps_unclassified;
    uint32_t traps_other;
    ac_trap_kind *trap_kinds;
    size_t trap_kind_count;
    uint32_t partitions_unreported;
    ac_partition *partitions;
    size_t partition_count;
} ac_report;

typedef struct {
    char *id;
    char *name;
    char *descriptor_symbol;
    ac_range *load_ranges;
    size_t load_range_count;
    ac_range *exec_ranges;
    size_t exec_range_count;
    ac_dispatch *dispatch;
    size_t dispatch_count;
    int entry_present;
    uint32_t entry_raw_address;
    uint32_t entry_dispatch_key;
    char *entry_function;
    int entry_resolved;
    int tls_present;
    int tls_resolved;
    uint32_t tls_address;
    uint32_t tls_file_size;
    uint32_t tls_memory_size;
    uint32_t tls_alignment;
    uint32_t tls_order;
    uint32_t tls_offset;
    ac_function_import *function_imports;
    size_t function_import_count;
    ac_export *exports;
    size_t export_count;
    uint32_t function_export_count;
    uint32_t variable_export_count;
    uint32_t tls_variable_export_count;
    ac_variable_import *variable_imports;
    size_t variable_import_count;
    int summary_structural_ready;
    int summary_production_ready;
    int summary_strict_unsupported;
    int summary_pc_instrumentation;
    uint32_t summary_unclassified_traps;
    ac_report report;
    int has_report;
} ac_module;

typedef struct {
    uint32_t key;
    const char *symbol;
    const char *module_id;
} ac_merged_dispatch;

typedef struct {
    const char *kind;
    uint64_t count;
} ac_merged_trap;

typedef struct {
    uint64_t load_ranges;
    uint64_t exec_ranges;
    uint64_t dispatch_entries;
    uint64_t functions_emitted;
    uint64_t functions_discovered;
    uint64_t functions_dropped;
    uint64_t functions_promoted;
    uint64_t instructions_total;
    uint64_t instructions_translated;
    uint64_t instructions_trapped;
    uint64_t body_cuts;
    uint64_t traps_total;
    uint64_t traps_classified;
    uint64_t traps_unclassified;
    uint64_t traps_other;
    uint64_t partitions_reported;
    uint64_t partitions_unreported;
    uint64_t tls_modules;
    uint64_t function_imports;
    uint64_t function_exports;
    uint64_t variable_exports;
    uint64_t tls_variable_exports;
    uint64_t variable_imports;
    uint64_t variable_sites;
} ac_totals;

static int ac_fail(char *error, size_t error_size, const char *fmt, ...) {
    if (error && error_size) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(error, error_size, fmt, ap);
        va_end(ap);
    }
    return 1;
}

static char *ac_strdup(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = (char *)malloc(size);
    if (copy) memcpy(copy, s, size);
    return copy;
}

static int ac_read_file(const char *path, ac_file *out,
                        char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    long length;
    size_t got;
    memset(out, 0, sizeof(*out));
    if (!file)
        return ac_fail(error, error_size, "cannot read input %s", path);
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        (uint64_t)length > AC_MAX_JSON_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ac_fail(error, error_size, "invalid or oversized input %s", path);
    }
    out->text = (char *)malloc((size_t)length + 1);
    if (!out->text) {
        fclose(file);
        return ac_fail(error, error_size, "out of memory reading %s", path);
    }
    got = fread(out->text, 1, (size_t)length, file);
    if (got != (size_t)length || fclose(file) != 0) {
        free(out->text);
        memset(out, 0, sizeof(*out));
        return ac_fail(error, error_size, "cannot read input %s", path);
    }
    out->text[got] = '\0';
    out->size = got;
    return 0;
}

static void jp_error(jparser *parser, const char *fmt, ...) {
    if (parser->failed) return;
    parser->failed = 1;
    if (parser->error && parser->error_size) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(parser->error, parser->error_size, fmt, ap);
        va_end(ap);
    }
}

static void jp_skip_ws(jparser *parser) {
    while (parser->cur < parser->end &&
           (*parser->cur == ' ' || *parser->cur == '\t' ||
            *parser->cur == '\r' || *parser->cur == '\n'))
        parser->cur++;
}

static void jv_free(jvalue *value) {
    size_t i;
    if (!value) return;
    if (value->kind == JV_STRING) {
        free(value->as.string);
    } else if (value->kind == JV_ARRAY) {
        for (i = 0; i < value->as.array.count; i++)
            jv_free(value->as.array.items[i]);
        free(value->as.array.items);
    } else if (value->kind == JV_OBJECT) {
        for (i = 0; i < value->as.object.count; i++) {
            free(value->as.object.members[i].name);
            jv_free(value->as.object.members[i].value);
        }
        free(value->as.object.members);
    }
    free(value);
}

static int jp_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int jp_append_utf8(char **buffer, size_t *length, size_t *capacity,
                          uint32_t codepoint) {
    unsigned char bytes[4];
    size_t count;
    char *grown;
    if (codepoint <= 0x7Fu) {
        bytes[0] = (unsigned char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7FFu) {
        bytes[0] = (unsigned char)(0xC0u | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        count = 2;
    } else if (codepoint <= 0xFFFFu) {
        bytes[0] = (unsigned char)(0xE0u | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        bytes[2] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        count = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0u | (codepoint >> 18));
        bytes[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        bytes[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        bytes[3] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        count = 4;
    }
    if (*length + count + 1 > *capacity) {
        size_t next = *capacity ? *capacity * 2 : 32;
        while (next < *length + count + 1) next *= 2;
        grown = (char *)realloc(*buffer, next);
        if (!grown) return 1;
        *buffer = grown;
        *capacity = next;
    }
    memcpy(*buffer + *length, bytes, count);
    *length += count;
    return 0;
}

static int jp_append_byte(char **buffer, size_t *length, size_t *capacity,
                          unsigned char byte) {
    char *grown;
    if (*length + 2 > *capacity) {
        size_t next = *capacity ? *capacity * 2 : 32;
        grown = (char *)realloc(*buffer, next);
        if (!grown) return 1;
        *buffer = grown;
        *capacity = next;
    }
    (*buffer)[(*length)++] = (char)byte;
    return 0;
}

static char *jp_string(jparser *parser) {
    char *buffer = NULL;
    size_t length = 0, capacity = 0;
    if (parser->cur >= parser->end || *parser->cur != '"') {
        jp_error(parser, "%s: expected JSON string", parser->path);
        return NULL;
    }
    parser->cur++;
    while (parser->cur < parser->end) {
        unsigned char c = (unsigned char)*parser->cur++;
        uint32_t codepoint;
        if (c == '"') {
            if (jp_append_utf8(&buffer, &length, &capacity, 0)) {
                jp_error(parser, "%s: out of memory parsing JSON", parser->path);
                free(buffer);
                return NULL;
            }
            return buffer;
        }
        if (c < 0x20u) {
            jp_error(parser, "%s: unescaped control byte in JSON string",
                     parser->path);
            free(buffer);
            return NULL;
        }
        if (c != '\\') {
            if (jp_append_byte(&buffer, &length, &capacity, c)) {
                jp_error(parser, "%s: out of memory parsing JSON", parser->path);
                free(buffer);
                return NULL;
            }
            continue;
        }
        if (parser->cur >= parser->end) {
            jp_error(parser, "%s: truncated JSON escape", parser->path);
            free(buffer);
            return NULL;
        }
        c = (unsigned char)*parser->cur++;
        if (c == '"' || c == '\\' || c == '/') codepoint = c;
        else if (c == 'b') codepoint = '\b';
        else if (c == 'f') codepoint = '\f';
        else if (c == 'n') codepoint = '\n';
        else if (c == 'r') codepoint = '\r';
        else if (c == 't') codepoint = '\t';
        else if (c == 'u') {
            int h0, h1, h2, h3;
            if ((size_t)(parser->end - parser->cur) < 4 ||
                (h0 = jp_hex_digit(parser->cur[0])) < 0 ||
                (h1 = jp_hex_digit(parser->cur[1])) < 0 ||
                (h2 = jp_hex_digit(parser->cur[2])) < 0 ||
                (h3 = jp_hex_digit(parser->cur[3])) < 0) {
                jp_error(parser, "%s: malformed JSON Unicode escape",
                         parser->path);
                free(buffer);
                return NULL;
            }
            codepoint = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
            parser->cur += 4;
            if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                uint32_t low;
                if ((size_t)(parser->end - parser->cur) < 6 ||
                    parser->cur[0] != '\\' || parser->cur[1] != 'u' ||
                    (h0 = jp_hex_digit(parser->cur[2])) < 0 ||
                    (h1 = jp_hex_digit(parser->cur[3])) < 0 ||
                    (h2 = jp_hex_digit(parser->cur[4])) < 0 ||
                    (h3 = jp_hex_digit(parser->cur[5])) < 0) {
                    jp_error(parser, "%s: unpaired JSON surrogate",
                             parser->path);
                    free(buffer);
                    return NULL;
                }
                low = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                if (low < 0xDC00u || low > 0xDFFFu) {
                    jp_error(parser, "%s: unpaired JSON surrogate",
                             parser->path);
                    free(buffer);
                    return NULL;
                }
                parser->cur += 6;
                codepoint = 0x10000u +
                    ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
            } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                jp_error(parser, "%s: unpaired JSON surrogate", parser->path);
                free(buffer);
                return NULL;
            }
        } else {
            jp_error(parser, "%s: unknown JSON escape", parser->path);
            free(buffer);
            return NULL;
        }
        if (codepoint == 0) {
            jp_error(parser, "%s: embedded NUL in JSON string", parser->path);
            free(buffer);
            return NULL;
        }
        if (jp_append_utf8(&buffer, &length, &capacity, codepoint)) {
            jp_error(parser, "%s: out of memory parsing JSON", parser->path);
            free(buffer);
            return NULL;
        }
    }
    jp_error(parser, "%s: unterminated JSON string", parser->path);
    free(buffer);
    return NULL;
}

static jvalue *jp_value(jparser *parser, unsigned depth);

static jvalue *jp_alloc(jparser *parser, jv_kind kind) {
    jvalue *value = (jvalue *)calloc(1, sizeof(*value));
    if (!value) {
        jp_error(parser, "%s: out of memory parsing JSON", parser->path);
        return NULL;
    }
    value->kind = kind;
    return value;
}

static jvalue *jp_array(jparser *parser, unsigned depth) {
    jvalue *value = jp_alloc(parser, JV_ARRAY);
    if (!value) return NULL;
    parser->cur++;
    jp_skip_ws(parser);
    if (parser->cur < parser->end && *parser->cur == ']') {
        parser->cur++;
        return value;
    }
    while (!parser->failed) {
        jvalue *item = jp_value(parser, depth + 1);
        jvalue **grown;
        if (!item) break;
        grown = (jvalue **)realloc(value->as.array.items,
                    (value->as.array.count + 1) * sizeof(*grown));
        if (!grown) {
            jv_free(item);
            jp_error(parser, "%s: out of memory parsing JSON", parser->path);
            break;
        }
        value->as.array.items = grown;
        value->as.array.items[value->as.array.count++] = item;
        jp_skip_ws(parser);
        if (parser->cur < parser->end && *parser->cur == ']') {
            parser->cur++;
            return value;
        }
        if (parser->cur >= parser->end || *parser->cur != ',') {
            jp_error(parser, "%s: expected ',' or ']' in JSON array",
                     parser->path);
            break;
        }
        parser->cur++;
        jp_skip_ws(parser);
    }
    jv_free(value);
    return NULL;
}

static jvalue *jp_object(jparser *parser, unsigned depth) {
    jvalue *value = jp_alloc(parser, JV_OBJECT);
    if (!value) return NULL;
    parser->cur++;
    jp_skip_ws(parser);
    if (parser->cur < parser->end && *parser->cur == '}') {
        parser->cur++;
        return value;
    }
    while (!parser->failed) {
        char *name;
        jvalue *member_value;
        jmember *grown;
        size_t i;
        name = jp_string(parser);
        if (!name) break;
        for (i = 0; i < value->as.object.count; i++) {
            if (strcmp(value->as.object.members[i].name, name) == 0) {
                jp_error(parser, "%s: duplicate JSON member '%s'",
                         parser->path, name);
                free(name);
                jv_free(value);
                return NULL;
            }
        }
        jp_skip_ws(parser);
        if (parser->cur >= parser->end || *parser->cur != ':') {
            jp_error(parser, "%s: expected ':' after JSON member '%s'",
                     parser->path, name);
            free(name);
            break;
        }
        parser->cur++;
        member_value = jp_value(parser, depth + 1);
        if (!member_value) {
            free(name);
            break;
        }
        grown = (jmember *)realloc(value->as.object.members,
                    (value->as.object.count + 1) * sizeof(*grown));
        if (!grown) {
            free(name);
            jv_free(member_value);
            jp_error(parser, "%s: out of memory parsing JSON", parser->path);
            break;
        }
        value->as.object.members = grown;
        grown[value->as.object.count].name = name;
        grown[value->as.object.count].value = member_value;
        value->as.object.count++;
        jp_skip_ws(parser);
        if (parser->cur < parser->end && *parser->cur == '}') {
            parser->cur++;
            return value;
        }
        if (parser->cur >= parser->end || *parser->cur != ',') {
            jp_error(parser, "%s: expected ',' or '}' in JSON object",
                     parser->path);
            break;
        }
        parser->cur++;
        jp_skip_ws(parser);
    }
    jv_free(value);
    return NULL;
}

static jvalue *jp_value(jparser *parser, unsigned depth) {
    jvalue *value;
    uint64_t number = 0;
    if (depth > AC_MAX_JSON_DEPTH) {
        jp_error(parser, "%s: JSON nesting exceeds %u",
                 parser->path, AC_MAX_JSON_DEPTH);
        return NULL;
    }
    jp_skip_ws(parser);
    if (parser->cur >= parser->end) {
        jp_error(parser, "%s: truncated JSON", parser->path);
        return NULL;
    }
    if (*parser->cur == '{') return jp_object(parser, depth);
    if (*parser->cur == '[') return jp_array(parser, depth);
    if (*parser->cur == '"') {
        value = jp_alloc(parser, JV_STRING);
        if (!value) return NULL;
        value->as.string = jp_string(parser);
        if (!value->as.string) {
            jv_free(value);
            return NULL;
        }
        return value;
    }
    if ((size_t)(parser->end - parser->cur) >= 4 &&
        memcmp(parser->cur, "true", 4) == 0) {
        parser->cur += 4;
        value = jp_alloc(parser, JV_BOOL);
        if (value) value->as.boolean = 1;
        return value;
    }
    if ((size_t)(parser->end - parser->cur) >= 5 &&
        memcmp(parser->cur, "false", 5) == 0) {
        parser->cur += 5;
        return jp_alloc(parser, JV_BOOL);
    }
    if ((size_t)(parser->end - parser->cur) >= 4 &&
        memcmp(parser->cur, "null", 4) == 0) {
        parser->cur += 4;
        return jp_alloc(parser, JV_NULL);
    }
    if (*parser->cur < '0' || *parser->cur > '9') {
        jp_error(parser, "%s: expected JSON value", parser->path);
        return NULL;
    }
    if (*parser->cur == '0' && parser->cur + 1 < parser->end &&
        parser->cur[1] >= '0' && parser->cur[1] <= '9') {
        jp_error(parser, "%s: JSON integer has a leading zero", parser->path);
        return NULL;
    }
    while (parser->cur < parser->end &&
           *parser->cur >= '0' && *parser->cur <= '9') {
        unsigned digit = (unsigned)(*parser->cur - '0');
        if (number > (UINT64_MAX - digit) / 10u) {
            jp_error(parser, "%s: JSON integer overflows uint64", parser->path);
            return NULL;
        }
        number = number * 10u + digit;
        parser->cur++;
    }
    if (parser->cur < parser->end &&
        (*parser->cur == '.' || *parser->cur == 'e' ||
         *parser->cur == 'E' || *parser->cur == '-')) {
        jp_error(parser, "%s: only unsigned integer JSON numbers are allowed",
                 parser->path);
        return NULL;
    }
    value = jp_alloc(parser, JV_UINT);
    if (value) value->as.uint_value = number;
    return value;
}

static jvalue *jp_document(const char *path, const ac_file *file,
                           char *error, size_t error_size) {
    jparser parser;
    jvalue *root;
    memset(&parser, 0, sizeof(parser));
    parser.cur = file->text;
    parser.end = file->text + file->size;
    parser.path = path;
    parser.error = error;
    parser.error_size = error_size;
    root = jp_value(&parser, 0);
    if (!root) return NULL;
    jp_skip_ws(&parser);
    if (parser.cur != parser.end) {
        ac_fail(error, error_size, "%s: trailing bytes after JSON document",
                path);
        jv_free(root);
        return NULL;
    }
    return root;
}

static const jvalue *jv_member(const jvalue *object, const char *name) {
    size_t i;
    if (!object || object->kind != JV_OBJECT) return NULL;
    for (i = 0; i < object->as.object.count; i++)
        if (strcmp(object->as.object.members[i].name, name) == 0)
            return object->as.object.members[i].value;
    return NULL;
}

static int jv_exact(const char *path, const char *where, const jvalue *object,
                    const char *const *names, size_t name_count,
                    char *error, size_t error_size) {
    size_t i, j;
    if (!object || object->kind != JV_OBJECT)
        return ac_fail(error, error_size, "%s: %s must be an object",
                       path, where);
    if (object->as.object.count != name_count)
        return ac_fail(error, error_size,
                       "%s: %s has missing or unknown members", path, where);
    for (i = 0; i < object->as.object.count; i++) {
        int known = 0;
        for (j = 0; j < name_count; j++)
            if (strcmp(object->as.object.members[i].name, names[j]) == 0)
                known = 1;
        if (!known)
            return ac_fail(error, error_size, "%s: %s has unknown member '%s'",
                           path, where, object->as.object.members[i].name);
    }
    return 0;
}

static int jv_uint32(const char *path, const char *where, const jvalue *value,
                     uint32_t *out, char *error, size_t error_size) {
    if (!value || value->kind != JV_UINT || value->as.uint_value > UINT32_MAX)
        return ac_fail(error, error_size, "%s: %s must be a uint32",
                       path, where);
    *out = (uint32_t)value->as.uint_value;
    return 0;
}

static int jv_uint64(const char *path, const char *where, const jvalue *value,
                     uint64_t *out, char *error, size_t error_size) {
    if (!value || value->kind != JV_UINT)
        return ac_fail(error, error_size, "%s: %s must be a uint64",
                       path, where);
    *out = value->as.uint_value;
    return 0;
}

static int jv_bool(const char *path, const char *where, const jvalue *value,
                   int *out, char *error, size_t error_size) {
    if (!value || value->kind != JV_BOOL)
        return ac_fail(error, error_size, "%s: %s must be boolean",
                       path, where);
    *out = value->as.boolean;
    return 0;
}

static int jv_string_copy(const char *path, const char *where,
                          const jvalue *value, int nullable, char **out,
                          char *error, size_t error_size) {
    if (nullable && value && value->kind == JV_NULL) {
        *out = NULL;
        return 0;
    }
    if (!value || value->kind != JV_STRING)
        return ac_fail(error, error_size, "%s: %s must be %sa string",
                       path, where, nullable ? "null or " : "");
    *out = ac_strdup(value->as.string);
    if (!*out)
        return ac_fail(error, error_size, "%s: out of memory", path);
    return 0;
}

static int jv_literal(const char *path, const char *where,
                      const jvalue *value, const char *expected,
                      char *error, size_t error_size) {
    if (!value || value->kind != JV_STRING ||
        strcmp(value->as.string, expected) != 0)
        return ac_fail(error, error_size, "%s: %s must be \"%s\"",
                       path, where, expected);
    return 0;
}

static int jv_hex32(const char *path, const char *where, const jvalue *value,
                    uint32_t *out, char *error, size_t error_size) {
    const char *s;
    uint32_t result = 0;
    size_t i;
    if (!value || value->kind != JV_STRING) goto invalid;
    s = value->as.string;
    if (strlen(s) != 10 || s[0] != '0' || s[1] != 'x') goto invalid;
    for (i = 2; i < 10; i++) {
        int digit = jp_hex_digit(s[i]);
        if (digit < 0 || (s[i] >= 'a' && s[i] <= 'f')) goto invalid;
        result = (result << 4) | (uint32_t)digit;
    }
    *out = result;
    return 0;
invalid:
    return ac_fail(error, error_size,
                   "%s: %s must be 0x plus eight uppercase hex digits",
                   path, where);
}

static int ac_module_id_valid(const char *id) {
    size_t i, length;
    if (!id) return 0;
    length = strlen(id);
    if (!length || length > EM_MODULE_ID_MAX) return 0;
    if (!((id[0] >= 'A' && id[0] <= 'Z') ||
          (id[0] >= 'a' && id[0] <= 'z')))
        return 0;
    for (i = 1; i < length; i++)
        if (!((id[i] >= 'A' && id[i] <= 'Z') ||
              (id[i] >= 'a' && id[i] <= 'z') ||
              (id[i] >= '0' && id[i] <= '9') || id[i] == '_'))
            return 0;
    return 1;
}

static int ac_c_identifier(const char *symbol) {
    size_t i;
    if (!symbol || !*symbol) return 0;
    if (!((symbol[0] >= 'A' && symbol[0] <= 'Z') ||
          (symbol[0] >= 'a' && symbol[0] <= 'z') || symbol[0] == '_'))
        return 0;
    for (i = 1; symbol[i]; i++)
        if (!((symbol[i] >= 'A' && symbol[i] <= 'Z') ||
              (symbol[i] >= 'a' && symbol[i] <= 'z') ||
              (symbol[i] >= '0' && symbol[i] <= '9') || symbol[i] == '_'))
            return 0;
    return 1;
}

static void ac_report_free(ac_report *report) {
    size_t i;
    free(report->module_id);
    free(report->module_name);
    for (i = 0; i < report->trap_kind_count; i++)
        free(report->trap_kinds[i].kind);
    free(report->trap_kinds);
    for (i = 0; i < report->partition_count; i++)
        free(report->partitions[i].unit);
    free(report->partitions);
    memset(report, 0, sizeof(*report));
}

static void ac_module_free(ac_module *module) {
    size_t i;
    free(module->id);
    free(module->name);
    free(module->descriptor_symbol);
    free(module->load_ranges);
    free(module->exec_ranges);
    for (i = 0; i < module->dispatch_count; i++)
        free(module->dispatch[i].symbol);
    free(module->dispatch);
    free(module->entry_function);
    for (i = 0; i < module->function_import_count; i++) {
        free(module->function_imports[i].name);
        free(module->function_imports[i].symbol);
        free(module->function_imports[i].target_slot);
    }
    free(module->function_imports);
    free(module->exports);
    for (i = 0; i < module->variable_import_count; i++) {
        free(module->variable_imports[i].name);
        free(module->variable_imports[i].symbol);
        free(module->variable_imports[i].sites);
    }
    free(module->variable_imports);
    ac_report_free(&module->report);
    memset(module, 0, sizeof(*module));
}

static int ac_parse_ranges(const char *path, const jvalue *array,
                           ac_range **out, size_t *out_count,
                           char *error, size_t error_size) {
    static const char *const names[] = {"begin", "endExclusive"};
    size_t i;
    ac_range *ranges;
    if (!array || array->kind != JV_ARRAY || !array->as.array.count)
        return ac_fail(error, error_size,
                       "%s: range array must be non-empty", path);
    if (array->as.array.count > UINT32_MAX)
        return ac_fail(error, error_size, "%s: too many ranges", path);
    ranges = (ac_range *)calloc(array->as.array.count, sizeof(*ranges));
    if (!ranges) return ac_fail(error, error_size, "%s: out of memory", path);
    for (i = 0; i < array->as.array.count; i++) {
        const jvalue *item = array->as.array.items[i];
        if (jv_exact(path, "range", item, names, 2, error, error_size) ||
            jv_hex32(path, "range.begin", jv_member(item, "begin"),
                     &ranges[i].begin, error, error_size) ||
            jv_hex32(path, "range.endExclusive",
                     jv_member(item, "endExclusive"), &ranges[i].end,
                     error, error_size) ||
            ranges[i].begin >= ranges[i].end) {
            if (!error || !error_size || !error[0])
                ac_fail(error, error_size, "%s: range is empty or inverted",
                        path);
            free(ranges);
            return 1;
        }
    }
    *out = ranges;
    *out_count = array->as.array.count;
    return 0;
}

static int ac_parse_sidecar(const char *path, const jvalue *root,
                            ac_module *module,
                            char *error, size_t error_size) {
    static const char *const root_names[] = {
        "schemaVersion", "abiVersion", "generator", "module", "ranges",
        "dispatch", "entry", "tls", "functionImports", "exports",
        "variableImports", "report"
    };
    static const char *const module_names[] = {
        "id", "name", "descriptorSymbol"
    };
    static const char *const ranges_names[] = {"load", "executable"};
    static const char *const collection_names[] = {"count", "items"};
    static const char *const dispatch_names[] = {"count", "entries"};
    static const char *const entry_names[] = {
        "present", "rawAddress", "dispatchKey", "function", "resolved"
    };
    static const char *const tls_names[] = {
        "present", "resolved", "address", "fileSize", "memorySize",
        "alignment"
    };
    static const char *const export_names[] = {
        "count", "functions", "variables", "tlsVariables", "items"
    };
    static const char *const summary_names[] = {
        "schemaVersion", "structuralReady", "productionReady",
        "strictUnsupported", "pcInstrumentation", "unclassifiedTrapCount"
    };
    const jvalue *object, *items;
    uint32_t value, declared;
    size_t i;

    memset(module, 0, sizeof(*module));
    if (jv_exact(path, "module sidecar", root, root_names, 12,
                 error, error_size) ||
        jv_uint32(path, "schemaVersion", jv_member(root, "schemaVersion"),
                  &value, error, error_size) ||
        value != VITA_GENERATED_REPORT_SCHEMA_VERSION ||
        jv_uint32(path, "abiVersion", jv_member(root, "abiVersion"),
                  &value, error, error_size) ||
        value != VITA_GENERATED_MODULE_ABI_VERSION ||
        jv_literal(path, "generator", jv_member(root, "generator"),
                   "armrecomp", error, error_size))
        goto fail;

    object = jv_member(root, "module");
    if (jv_exact(path, "module", object, module_names, 3,
                 error, error_size) ||
        jv_string_copy(path, "module.id", jv_member(object, "id"), 0,
                       &module->id, error, error_size) ||
        jv_string_copy(path, "module.name", jv_member(object, "name"), 0,
                       &module->name, error, error_size) ||
        jv_string_copy(path, "module.descriptorSymbol",
                       jv_member(object, "descriptorSymbol"), 0,
                       &module->descriptor_symbol, error, error_size))
        goto fail;
    if (!ac_module_id_valid(module->id))
        return ac_fail(error, error_size, "%s: invalid module ID '%s'",
                       path, module->id);
    {
        char expected[128];
        (void)snprintf(expected, sizeof(expected),
                       "vita_generated_module_%s", module->id);
        if (strcmp(module->descriptor_symbol, expected) != 0)
            return ac_fail(error, error_size,
                           "%s: descriptorSymbol is not collision-safe for module '%s'",
                           path, module->id);
    }

    object = jv_member(root, "ranges");
    if (jv_exact(path, "ranges", object, ranges_names, 2,
                 error, error_size) ||
        ac_parse_ranges(path, jv_member(object, "load"),
                        &module->load_ranges, &module->load_range_count,
                        error, error_size) ||
        ac_parse_ranges(path, jv_member(object, "executable"),
                        &module->exec_ranges, &module->exec_range_count,
                        error, error_size))
        goto fail;

    object = jv_member(root, "dispatch");
    if (jv_exact(path, "dispatch", object, dispatch_names, 2,
                 error, error_size) ||
        jv_uint32(path, "dispatch.count", jv_member(object, "count"),
                  &declared, error, error_size))
        goto fail;
    items = jv_member(object, "entries");
    if (!items || items->kind != JV_ARRAY || items->as.array.count != declared ||
        !declared)
        return ac_fail(error, error_size,
                       "%s: dispatch count/items mismatch or empty dispatch",
                       path);
    module->dispatch = (ac_dispatch *)calloc(declared,
                                               sizeof(*module->dispatch));
    if (!module->dispatch)
        return ac_fail(error, error_size, "%s: out of memory", path);
    module->dispatch_count = declared;
    for (i = 0; i < declared; i++) {
        static const char *const function_fields[] = {
            "key", "kind", "symbol"
        };
        static const char *const import_fields[] = {
            "key", "kind", "symbol", "libraryNid", "functionNid"
        };
        const jvalue *item = items->as.array.items[i];
        const jvalue *kind = jv_member(item, "kind");
        ac_dispatch *dispatch = &module->dispatch[i];
        if (!kind || kind->kind != JV_STRING)
            return ac_fail(error, error_size,
                           "%s: dispatch kind must be a string", path);
        if (strcmp(kind->as.string, "function") == 0) {
            dispatch->kind = AC_DISPATCH_FUNCTION;
            if (jv_exact(path, "dispatch function", item, function_fields, 3,
                         error, error_size))
                goto fail;
        } else if (strcmp(kind->as.string, "functionImport") == 0) {
            dispatch->kind = AC_DISPATCH_IMPORT;
            if (jv_exact(path, "dispatch import", item, import_fields, 5,
                         error, error_size) ||
                jv_hex32(path, "dispatch.libraryNid",
                         jv_member(item, "libraryNid"),
                         &dispatch->library_nid, error, error_size) ||
                jv_hex32(path, "dispatch.functionNid",
                         jv_member(item, "functionNid"),
                         &dispatch->function_nid, error, error_size))
                goto fail;
        } else {
            return ac_fail(error, error_size,
                           "%s: unknown dispatch kind '%s'",
                           path, kind->as.string);
        }
        if (jv_hex32(path, "dispatch.key", jv_member(item, "key"),
                     &dispatch->key, error, error_size) ||
            jv_string_copy(path, "dispatch.symbol", jv_member(item, "symbol"),
                           0, &dispatch->symbol, error, error_size))
            goto fail;
        if (!ac_c_identifier(dispatch->symbol))
            return ac_fail(error, error_size,
                           "%s: invalid dispatch C symbol '%s'",
                           path, dispatch->symbol);
    }

    object = jv_member(root, "entry");
    if (jv_exact(path, "entry", object, entry_names, 5, error, error_size) ||
        jv_bool(path, "entry.present", jv_member(object, "present"),
                &module->entry_present, error, error_size) ||
        jv_hex32(path, "entry.rawAddress", jv_member(object, "rawAddress"),
                 &module->entry_raw_address, error, error_size) ||
        jv_hex32(path, "entry.dispatchKey",
                 jv_member(object, "dispatchKey"),
                 &module->entry_dispatch_key, error, error_size) ||
        jv_string_copy(path, "entry.function", jv_member(object, "function"),
                       1, &module->entry_function, error, error_size) ||
        jv_bool(path, "entry.resolved", jv_member(object, "resolved"),
                &module->entry_resolved, error, error_size))
        goto fail;

    object = jv_member(root, "tls");
    if (jv_exact(path, "tls", object, tls_names, 6, error, error_size) ||
        jv_bool(path, "tls.present", jv_member(object, "present"),
                &module->tls_present, error, error_size) ||
        jv_bool(path, "tls.resolved", jv_member(object, "resolved"),
                &module->tls_resolved, error, error_size) ||
        jv_hex32(path, "tls.address", jv_member(object, "address"),
                 &module->tls_address, error, error_size) ||
        jv_uint32(path, "tls.fileSize", jv_member(object, "fileSize"),
                  &module->tls_file_size, error, error_size) ||
        jv_uint32(path, "tls.memorySize", jv_member(object, "memorySize"),
                  &module->tls_memory_size, error, error_size) ||
        jv_uint32(path, "tls.alignment", jv_member(object, "alignment"),
                  &module->tls_alignment, error, error_size))
        goto fail;

    object = jv_member(root, "functionImports");
    if (jv_exact(path, "functionImports", object, collection_names, 2,
                 error, error_size) ||
        jv_uint32(path, "functionImports.count", jv_member(object, "count"),
                  &declared, error, error_size))
        goto fail;
    items = jv_member(object, "items");
    if (!items || items->kind != JV_ARRAY || items->as.array.count != declared)
        return ac_fail(error, error_size,
                       "%s: functionImports count/items mismatch", path);
    module->function_imports = (ac_function_import *)calloc(
        declared ? declared : 1, sizeof(*module->function_imports));
    if (!module->function_imports)
        return ac_fail(error, error_size, "%s: out of memory", path);
    module->function_import_count = declared;
    for (i = 0; i < declared; i++) {
        static const char *const fields[] = {
            "key", "stubAddress", "targetSite", "libraryNid", "functionNid",
            "name", "symbol", "targetSlot", "strictUnsupported"
        };
        const jvalue *item = items->as.array.items[i];
        ac_function_import *import = &module->function_imports[i];
        if (jv_exact(path, "function import", item, fields, 9,
                     error, error_size) ||
            jv_hex32(path, "functionImport.key", jv_member(item, "key"),
                     &import->key, error, error_size) ||
            jv_hex32(path, "functionImport.stubAddress",
                     jv_member(item, "stubAddress"), &import->stub_address,
                     error, error_size) ||
            jv_hex32(path, "functionImport.targetSite",
                     jv_member(item, "targetSite"), &import->target_site,
                     error, error_size) ||
            jv_hex32(path, "functionImport.libraryNid",
                     jv_member(item, "libraryNid"), &import->library_nid,
                     error, error_size) ||
            jv_hex32(path, "functionImport.functionNid",
                     jv_member(item, "functionNid"), &import->function_nid,
                     error, error_size) ||
            jv_string_copy(path, "functionImport.name",
                           jv_member(item, "name"), 1, &import->name,
                           error, error_size) ||
            jv_string_copy(path, "functionImport.symbol",
                           jv_member(item, "symbol"), 0, &import->symbol,
                           error, error_size) ||
            jv_string_copy(path, "functionImport.targetSlot",
                           jv_member(item, "targetSlot"), 0,
                           &import->target_slot, error, error_size) ||
            jv_bool(path, "functionImport.strictUnsupported",
                    jv_member(item, "strictUnsupported"),
                    &import->strict_unsupported, error, error_size))
            goto fail;
        if (!ac_c_identifier(import->symbol) ||
            !ac_c_identifier(import->target_slot))
            return ac_fail(error, error_size,
                           "%s: invalid function-import symbol", path);
    }

    object = jv_member(root, "exports");
    if (jv_exact(path, "exports", object, export_names, 5,
                 error, error_size) ||
        jv_uint32(path, "exports.count", jv_member(object, "count"),
                  &declared, error, error_size) ||
        jv_uint32(path, "exports.functions", jv_member(object, "functions"),
                  &module->function_export_count, error, error_size) ||
        jv_uint32(path, "exports.variables", jv_member(object, "variables"),
                  &module->variable_export_count, error, error_size) ||
        jv_uint32(path, "exports.tlsVariables",
                  jv_member(object, "tlsVariables"),
                  &module->tls_variable_export_count, error, error_size))
        goto fail;
    items = jv_member(object, "items");
    if (!items || items->kind != JV_ARRAY || items->as.array.count != declared)
        return ac_fail(error, error_size,
                       "%s: exports count/items mismatch", path);
    module->exports = (ac_export *)calloc(declared ? declared : 1,
                                          sizeof(*module->exports));
    if (!module->exports)
        return ac_fail(error, error_size, "%s: out of memory", path);
    module->export_count = declared;
    for (i = 0; i < declared; i++) {
        static const char *const fields[] = {
            "kind", "libraryNid", "nid", "address", "dispatchKey"
        };
        const jvalue *item = items->as.array.items[i];
        const jvalue *kind = jv_member(item, "kind");
        const jvalue *key = jv_member(item, "dispatchKey");
        ac_export *export_item = &module->exports[i];
        if (jv_exact(path, "export", item, fields, 5, error, error_size) ||
            !kind || kind->kind != JV_STRING ||
            jv_hex32(path, "export.libraryNid",
                     jv_member(item, "libraryNid"), &export_item->library_nid,
                     error, error_size) ||
            jv_hex32(path, "export.nid", jv_member(item, "nid"),
                     &export_item->nid, error, error_size) ||
            jv_hex32(path, "export.address", jv_member(item, "address"),
                     &export_item->address, error, error_size))
            goto fail;
        if (strcmp(kind->as.string, "function") == 0)
            export_item->kind = AC_EXPORT_FUNCTION;
        else if (strcmp(kind->as.string, "variable") == 0)
            export_item->kind = AC_EXPORT_VARIABLE;
        else if (strcmp(kind->as.string, "tlsVariable") == 0)
            export_item->kind = AC_EXPORT_TLS_VARIABLE;
        else
            return ac_fail(error, error_size,
                           "%s: unknown export kind", path);
        if (export_item->kind == AC_EXPORT_FUNCTION) {
            export_item->has_dispatch_key = 1;
            if (jv_hex32(path, "export.dispatchKey", key,
                         &export_item->dispatch_key, error, error_size))
                goto fail;
        } else if (!key || key->kind != JV_NULL) {
            return ac_fail(error, error_size,
                           "%s: non-function export dispatchKey must be null",
                           path);
        }
    }

    object = jv_member(root, "variableImports");
    if (jv_exact(path, "variableImports", object, collection_names, 2,
                 error, error_size) ||
        jv_uint32(path, "variableImports.count", jv_member(object, "count"),
                  &declared, error, error_size))
        goto fail;
    items = jv_member(object, "items");
    if (!items || items->kind != JV_ARRAY || items->as.array.count != declared)
        return ac_fail(error, error_size,
                       "%s: variableImports count/items mismatch", path);
    module->variable_imports = (ac_variable_import *)calloc(
        declared ? declared : 1, sizeof(*module->variable_imports));
    if (!module->variable_imports)
        return ac_fail(error, error_size, "%s: out of memory", path);
    module->variable_import_count = declared;
    for (i = 0; i < declared; i++) {
        static const char *const fields[] = {
            "libraryNid", "variableNid", "stubAddress", "name", "symbol", "sites"
        };
        static const char *const site_fields[] = {
            "address", "addend", "relocationCode"
        };
        const jvalue *item = items->as.array.items[i];
        const jvalue *sites;
        size_t s;
        ac_variable_import *import = &module->variable_imports[i];
        if (jv_exact(path, "variable import", item, fields, 6,
                     error, error_size) ||
            jv_hex32(path, "variableImport.libraryNid",
                     jv_member(item, "libraryNid"), &import->library_nid,
                     error, error_size) ||
            jv_hex32(path, "variableImport.variableNid",
                     jv_member(item, "variableNid"), &import->variable_nid,
                     error, error_size) ||
            jv_hex32(path, "variableImport.stubAddress",
                     jv_member(item, "stubAddress"), &import->stub_address,
                     error, error_size) ||
            jv_string_copy(path, "variableImport.name",
                           jv_member(item, "name"), 1, &import->name,
                           error, error_size) ||
            jv_string_copy(path, "variableImport.symbol",
                           jv_member(item, "symbol"), 0, &import->symbol,
                           error, error_size))
            goto fail;
        if (!ac_c_identifier(import->symbol))
            return ac_fail(error, error_size,
                           "%s: invalid variable-import symbol", path);
        sites = jv_member(item, "sites");
        if (!sites || sites->kind != JV_ARRAY)
            return ac_fail(error, error_size,
                           "%s: variable import sites must be an array", path);
        import->site_count = sites->as.array.count;
        import->sites = (ac_variable_site *)calloc(
            import->site_count ? import->site_count : 1,
            sizeof(*import->sites));
        if (!import->sites)
            return ac_fail(error, error_size, "%s: out of memory", path);
        for (s = 0; s < import->site_count; s++) {
            const jvalue *site = sites->as.array.items[s];
            if (jv_exact(path, "variable import site", site, site_fields, 3,
                         error, error_size) ||
                jv_hex32(path, "variableImport.site.address",
                         jv_member(site, "address"),
                         &import->sites[s].address, error, error_size) ||
                jv_uint32(path, "variableImport.site.addend",
                          jv_member(site, "addend"),
                          &import->sites[s].addend, error, error_size) ||
                jv_uint32(path, "variableImport.site.relocationCode",
                          jv_member(site, "relocationCode"),
                          &import->sites[s].relocation_code,
                          error, error_size))
                goto fail;
        }
    }

    object = jv_member(root, "report");
    if (jv_exact(path, "report summary", object, summary_names, 6,
                 error, error_size) ||
        jv_uint32(path, "report.schemaVersion",
                  jv_member(object, "schemaVersion"), &value,
                  error, error_size) ||
        value != VITA_GENERATED_REPORT_SCHEMA_VERSION ||
        jv_bool(path, "report.structuralReady",
                jv_member(object, "structuralReady"),
                &module->summary_structural_ready, error, error_size) ||
        jv_bool(path, "report.productionReady",
                jv_member(object, "productionReady"),
                &module->summary_production_ready, error, error_size) ||
        jv_bool(path, "report.strictUnsupported",
                jv_member(object, "strictUnsupported"),
                &module->summary_strict_unsupported, error, error_size) ||
        jv_bool(path, "report.pcInstrumentation",
                jv_member(object, "pcInstrumentation"),
                &module->summary_pc_instrumentation, error, error_size) ||
        jv_uint32(path, "report.unclassifiedTrapCount",
                  jv_member(object, "unclassifiedTrapCount"),
                  &module->summary_unclassified_traps, error, error_size))
        goto fail;
    if (!module->summary_strict_unsupported)
        return ac_fail(error, error_size,
                       "%s: strictUnsupported must be true", path);
    return 0;

fail:
    ac_module_free(module);
    if (error && error_size && !error[0])
        ac_fail(error, error_size, "%s: unsupported module-sidecar schema",
                path);
    return 1;
}

static int ac_parse_report(const char *path, const jvalue *root,
                           ac_report *report,
                           char *error, size_t error_size) {
    static const char *const root_names[] = {
        "schemaVersion", "generator", "module", "strictUnsupported",
        "structuralReady", "productionReady", "instrumentation", "entry",
        "tls", "functions", "instructions", "traps", "partitions"
    };
    static const char *const module_names[] = {"id", "name"};
    static const char *const instrumentation_names[] = {"functionEntry", "pc"};
    static const char *const entry_names[] = {
        "present", "rawAddress", "dispatchKey", "resolved"
    };
    static const char *const tls_names[] = {
        "present", "resolved", "address", "fileSize", "memorySize",
        "alignment"
    };
    static const char *const function_names[] = {
        "emitted", "discovered", "dropped", "promotedWeakRoots"
    };
    static const char *const instruction_names[] = {
        "total", "translated", "trapped", "bodyCuts"
    };
    static const char *const trap_names[] = {
        "total", "classified", "unclassified", "other", "kinds"
    };
    static const char *const trap_kind_names[] = {"kind", "count"};
    static const char *const partition_names[] = {"unreported", "items"};
    static const char *const partition_item_names[] = {
        "unit", "functions", "instructions", "firstKey", "lastKey"
    };
    const jvalue *object, *items;
    uint32_t schema;
    size_t i;
    int strict;

    memset(report, 0, sizeof(*report));
    if (jv_exact(path, "generation report", root, root_names, 13,
                 error, error_size) ||
        jv_uint32(path, "schemaVersion", jv_member(root, "schemaVersion"),
                  &schema, error, error_size) ||
        schema != VITA_GENERATED_REPORT_SCHEMA_VERSION ||
        jv_literal(path, "generator", jv_member(root, "generator"),
                   "armrecomp", error, error_size) ||
        jv_bool(path, "strictUnsupported",
                jv_member(root, "strictUnsupported"), &strict,
                error, error_size) || !strict)
        goto fail;

    object = jv_member(root, "module");
    if (jv_exact(path, "module", object, module_names, 2,
                 error, error_size) ||
        jv_string_copy(path, "module.id", jv_member(object, "id"), 0,
                       &report->module_id, error, error_size) ||
        jv_string_copy(path, "module.name", jv_member(object, "name"), 0,
                       &report->module_name, error, error_size) ||
        !ac_module_id_valid(report->module_id))
        goto fail;

    if (jv_bool(path, "structuralReady", jv_member(root, "structuralReady"),
                &report->structural_ready, error, error_size) ||
        jv_bool(path, "productionReady", jv_member(root, "productionReady"),
                &report->production_ready, error, error_size))
        goto fail;

    object = jv_member(root, "instrumentation");
    if (jv_exact(path, "instrumentation", object, instrumentation_names, 2,
                 error, error_size) ||
        jv_bool(path, "instrumentation.functionEntry",
                jv_member(object, "functionEntry"),
                &report->instrumentation_function_entry,
                error, error_size) ||
        jv_bool(path, "instrumentation.pc", jv_member(object, "pc"),
                &report->instrumentation_pc, error, error_size))
        goto fail;
    if (!report->instrumentation_function_entry)
        return ac_fail(error, error_size,
                       "%s: function-entry instrumentation is required", path);

    object = jv_member(root, "entry");
    if (jv_exact(path, "entry", object, entry_names, 4,
                 error, error_size) ||
        jv_bool(path, "entry.present", jv_member(object, "present"),
                &report->entry_present, error, error_size) ||
        jv_hex32(path, "entry.rawAddress", jv_member(object, "rawAddress"),
                 &report->entry_raw_address, error, error_size) ||
        jv_hex32(path, "entry.dispatchKey",
                 jv_member(object, "dispatchKey"),
                 &report->entry_dispatch_key, error, error_size) ||
        jv_bool(path, "entry.resolved", jv_member(object, "resolved"),
                &report->entry_resolved, error, error_size))
        goto fail;

    object = jv_member(root, "tls");
    if (jv_exact(path, "tls", object, tls_names, 6,
                 error, error_size) ||
        jv_bool(path, "tls.present", jv_member(object, "present"),
                &report->tls_present, error, error_size) ||
        jv_bool(path, "tls.resolved", jv_member(object, "resolved"),
                &report->tls_resolved, error, error_size) ||
        jv_hex32(path, "tls.address", jv_member(object, "address"),
                 &report->tls_address, error, error_size) ||
        jv_uint32(path, "tls.fileSize", jv_member(object, "fileSize"),
                  &report->tls_file_size, error, error_size) ||
        jv_uint32(path, "tls.memorySize", jv_member(object, "memorySize"),
                  &report->tls_memory_size, error, error_size) ||
        jv_uint32(path, "tls.alignment", jv_member(object, "alignment"),
                  &report->tls_alignment, error, error_size))
        goto fail;

    object = jv_member(root, "functions");
    if (jv_exact(path, "functions", object, function_names, 4,
                 error, error_size) ||
        jv_uint32(path, "functions.emitted", jv_member(object, "emitted"),
                  &report->functions_emitted, error, error_size) ||
        jv_uint32(path, "functions.discovered",
                  jv_member(object, "discovered"),
                  &report->functions_discovered, error, error_size) ||
        jv_uint32(path, "functions.dropped", jv_member(object, "dropped"),
                  &report->functions_dropped, error, error_size) ||
        jv_uint32(path, "functions.promotedWeakRoots",
                  jv_member(object, "promotedWeakRoots"),
                  &report->functions_promoted, error, error_size))
        goto fail;

    object = jv_member(root, "instructions");
    if (jv_exact(path, "instructions", object, instruction_names, 4,
                 error, error_size) ||
        jv_uint32(path, "instructions.total", jv_member(object, "total"),
                  &report->instructions_total, error, error_size) ||
        jv_uint32(path, "instructions.translated",
                  jv_member(object, "translated"),
                  &report->instructions_translated, error, error_size) ||
        jv_uint32(path, "instructions.trapped", jv_member(object, "trapped"),
                  &report->instructions_trapped, error, error_size) ||
        jv_uint32(path, "instructions.bodyCuts", jv_member(object, "bodyCuts"),
                  &report->body_cuts, error, error_size))
        goto fail;

    object = jv_member(root, "traps");
    if (jv_exact(path, "traps", object, trap_names, 5,
                 error, error_size) ||
        jv_uint64(path, "traps.total", jv_member(object, "total"),
                  &report->traps_total, error, error_size) ||
        jv_uint64(path, "traps.classified", jv_member(object, "classified"),
                  &report->traps_classified, error, error_size) ||
        jv_uint32(path, "traps.unclassified",
                  jv_member(object, "unclassified"),
                  &report->traps_unclassified, error, error_size) ||
        jv_uint32(path, "traps.other", jv_member(object, "other"),
                  &report->traps_other, error, error_size))
        goto fail;
    items = jv_member(object, "kinds");
    if (!items || items->kind != JV_ARRAY)
        return ac_fail(error, error_size,
                       "%s: traps.kinds must be an array", path);
    report->trap_kind_count = items->as.array.count;
    report->trap_kinds = (ac_trap_kind *)calloc(
        report->trap_kind_count ? report->trap_kind_count : 1,
        sizeof(*report->trap_kinds));
    if (!report->trap_kinds)
        return ac_fail(error, error_size, "%s: out of memory", path);
    for (i = 0; i < report->trap_kind_count; i++) {
        const jvalue *item = items->as.array.items[i];
        if (jv_exact(path, "trap kind", item, trap_kind_names, 2,
                     error, error_size) ||
            jv_string_copy(path, "trap.kind", jv_member(item, "kind"), 0,
                           &report->trap_kinds[i].kind, error, error_size) ||
            !report->trap_kinds[i].kind[0] ||
            jv_uint64(path, "trap.count", jv_member(item, "count"),
                      &report->trap_kinds[i].count, error, error_size) ||
            !report->trap_kinds[i].count)
            goto fail;
    }

    object = jv_member(root, "partitions");
    if (jv_exact(path, "partitions", object, partition_names, 2,
                 error, error_size) ||
        jv_uint32(path, "partitions.unreported",
                  jv_member(object, "unreported"),
                  &report->partitions_unreported, error, error_size))
        goto fail;
    items = jv_member(object, "items");
    if (!items || items->kind != JV_ARRAY)
        return ac_fail(error, error_size,
                       "%s: partitions.items must be an array", path);
    report->partition_count = items->as.array.count;
    report->partitions = (ac_partition *)calloc(
        report->partition_count ? report->partition_count : 1,
        sizeof(*report->partitions));
    if (!report->partitions)
        return ac_fail(error, error_size, "%s: out of memory", path);
    for (i = 0; i < report->partition_count; i++) {
        const jvalue *item = items->as.array.items[i];
        if (jv_exact(path, "partition", item, partition_item_names, 5,
                     error, error_size) ||
            jv_string_copy(path, "partition.unit", jv_member(item, "unit"), 0,
                           &report->partitions[i].unit, error, error_size) ||
            jv_uint32(path, "partition.functions",
                      jv_member(item, "functions"),
                      &report->partitions[i].functions, error, error_size) ||
            jv_uint32(path, "partition.instructions",
                      jv_member(item, "instructions"),
                      &report->partitions[i].instructions,
                      error, error_size) ||
            jv_hex32(path, "partition.firstKey",
                     jv_member(item, "firstKey"),
                     &report->partitions[i].first_key,
                     error, error_size) ||
            jv_hex32(path, "partition.lastKey",
                     jv_member(item, "lastKey"),
                     &report->partitions[i].last_key,
                     error, error_size))
            goto fail;
    }
    return 0;

fail:
    ac_report_free(report);
    if (error && error_size && !error[0])
        ac_fail(error, error_size, "%s: unsupported generation-report schema",
                path);
    return 1;
}

static int ac_range_cmp(const void *left, const void *right) {
    const ac_range *a = (const ac_range *)left;
    const ac_range *b = (const ac_range *)right;
    if (a->begin != b->begin) return a->begin < b->begin ? -1 : 1;
    if (a->end != b->end) return a->end < b->end ? -1 : 1;
    return 0;
}

static int ac_dispatch_cmp(const void *left, const void *right) {
    const ac_merged_dispatch *a = (const ac_merged_dispatch *)left;
    const ac_merged_dispatch *b = (const ac_merged_dispatch *)right;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return strcmp(a->module_id, b->module_id);
}

static int ac_module_cmp(const void *left, const void *right) {
    const ac_module *a = (const ac_module *)left;
    const ac_module *b = (const ac_module *)right;
    uint32_t abegin = a->load_ranges[0].begin;
    uint32_t bbegin = b->load_ranges[0].begin;
    if (abegin != bbegin) return abegin < bbegin ? -1 : 1;
    return strcmp(a->id, b->id);
}

static int ac_align_up(uint32_t value, uint32_t alignment, uint32_t *out) {
    uint64_t aligned =
        ((uint64_t)value + alignment - 1u) &
        ~(uint64_t)(alignment - 1u);
    if (aligned > UINT32_MAX) return 1;
    *out = (uint32_t)aligned;
    return 0;
}

static int ac_trap_cmp(const void *left, const void *right) {
    const ac_merged_trap *a = (const ac_merged_trap *)left;
    const ac_merged_trap *b = (const ac_merged_trap *)right;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    return strcmp(a->kind, b->kind);
}

static int ac_import_cmp(const void *left, const void *right) {
    const ac_function_import *a = (const ac_function_import *)left;
    const ac_function_import *b = (const ac_function_import *)right;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    if (a->library_nid != b->library_nid)
        return a->library_nid < b->library_nid ? -1 : 1;
    if (a->function_nid != b->function_nid)
        return a->function_nid < b->function_nid ? -1 : 1;
    return strcmp(a->symbol, b->symbol);
}

static int ac_export_cmp(const void *left, const void *right) {
    const ac_export *a = (const ac_export *)left;
    const ac_export *b = (const ac_export *)right;
    if (a->kind != b->kind) return (int)a->kind - (int)b->kind;
    if (a->library_nid != b->library_nid)
        return a->library_nid < b->library_nid ? -1 : 1;
    if (a->nid != b->nid) return a->nid < b->nid ? -1 : 1;
    if (a->address != b->address) return a->address < b->address ? -1 : 1;
    return 0;
}

static int ac_variable_cmp(const void *left, const void *right) {
    const ac_variable_import *a = (const ac_variable_import *)left;
    const ac_variable_import *b = (const ac_variable_import *)right;
    if (a->library_nid != b->library_nid)
        return a->library_nid < b->library_nid ? -1 : 1;
    if (a->variable_nid != b->variable_nid)
        return a->variable_nid < b->variable_nid ? -1 : 1;
    return strcmp(a->symbol, b->symbol);
}

static int ac_site_cmp(const void *left, const void *right) {
    const ac_variable_site *a = (const ac_variable_site *)left;
    const ac_variable_site *b = (const ac_variable_site *)right;
    if (a->address != b->address) return a->address < b->address ? -1 : 1;
    if (a->addend != b->addend) return a->addend < b->addend ? -1 : 1;
    if (a->relocation_code != b->relocation_code)
        return a->relocation_code < b->relocation_code ? -1 : 1;
    return 0;
}

static int ac_in_ranges(uint32_t address, const ac_range *ranges,
                        size_t range_count) {
    size_t i;
    for (i = 0; i < range_count; i++)
        if (address >= ranges[i].begin && address < ranges[i].end)
            return 1;
    return 0;
}

static const ac_dispatch *ac_find_dispatch(const ac_module *module,
                                           uint32_t key, size_t *matches) {
    const ac_dispatch *found = NULL;
    size_t i;
    *matches = 0;
    for (i = 0; i < module->dispatch_count; i++) {
        if (module->dispatch[i].key == key) {
            found = &module->dispatch[i];
            (*matches)++;
        }
    }
    return found;
}

static int ac_validate_report(const ac_module *module,
                              char *error, size_t error_size) {
    const ac_report *report = &module->report;
    uint64_t classified = report->traps_other;
    uint64_t partition_functions = 0;
    uint64_t partition_instructions = 0;
    size_t i, j;
    int expected_production;

    if (strcmp(module->id, report->module_id) != 0 ||
        strcmp(module->name, report->module_name) != 0)
        return ac_fail(error, error_size,
                       "module/report identity mismatch for '%s'", module->id);
    if (module->summary_structural_ready != report->structural_ready ||
        module->summary_production_ready != report->production_ready ||
        module->summary_pc_instrumentation != report->instrumentation_pc ||
        module->summary_unclassified_traps != report->traps_unclassified)
        return ac_fail(error, error_size,
                       "module/report summary mismatch for '%s'", module->id);
    if (module->entry_present != report->entry_present ||
        module->entry_raw_address != report->entry_raw_address ||
        module->entry_dispatch_key != report->entry_dispatch_key ||
        module->entry_resolved != report->entry_resolved)
        return ac_fail(error, error_size,
                       "module/report entry mismatch for '%s'", module->id);
    if (module->tls_present != report->tls_present ||
        module->tls_resolved != report->tls_resolved ||
        module->tls_address != report->tls_address ||
        module->tls_file_size != report->tls_file_size ||
        module->tls_memory_size != report->tls_memory_size ||
        module->tls_alignment != report->tls_alignment)
        return ac_fail(error, error_size,
                       "module/report TLS mismatch for '%s'", module->id);
    if ((uint64_t)report->functions_emitted + report->functions_dropped !=
            report->functions_discovered ||
        report->functions_promoted > report->functions_emitted ||
        report->body_cuts > report->functions_emitted)
        return ac_fail(error, error_size,
                       "function totals do not balance for '%s'", module->id);
    if ((uint64_t)report->instructions_translated +
            report->instructions_trapped != report->instructions_total)
        return ac_fail(error, error_size,
                       "instruction totals do not balance for '%s'", module->id);
    for (i = 0; i < report->trap_kind_count; i++) {
        if (report->trap_kinds[i].count > UINT32_MAX ||
            UINT64_MAX - classified < report->trap_kinds[i].count)
            return ac_fail(error, error_size,
                           "trap count is out of range for '%s'", module->id);
        classified += report->trap_kinds[i].count;
        for (j = 0; j < i; j++)
            if (strcmp(report->trap_kinds[i].kind,
                       report->trap_kinds[j].kind) == 0)
                return ac_fail(error, error_size,
                               "duplicate trap kind '%s' in module '%s'",
                               report->trap_kinds[i].kind, module->id);
    }
    if (classified > UINT32_MAX ||
        report->traps_classified > UINT32_MAX ||
        report->traps_classified >
            UINT32_MAX - report->traps_unclassified ||
        classified != report->traps_classified ||
        report->traps_classified + report->traps_unclassified !=
            report->traps_total ||
        report->traps_total != report->instructions_trapped ||
        report->traps_total > UINT32_MAX)
        return ac_fail(error, error_size,
                       "trap totals do not balance for '%s'", module->id);
    if (report->partition_count > EM_MAX_PARTS)
        return ac_fail(error, error_size,
                       "partition count is out of range for '%s'", module->id);
    for (i = 0; i < report->partition_count; i++) {
        const ac_partition *partition = &report->partitions[i];
        size_t unit_length = strlen(partition->unit);
        int valid_unit = strcmp(partition->unit, "main") == 0 ||
            (unit_length == 8 &&
             strncmp(partition->unit, "part-", 5) == 0 &&
             partition->unit[5] >= '0' && partition->unit[5] <= '9' &&
             partition->unit[6] >= '0' && partition->unit[6] <= '9' &&
             partition->unit[7] >= '0' && partition->unit[7] <= '9');
        if (!valid_unit || !partition->functions ||
            (partition->functions &&
             partition->first_key > partition->last_key))
            return ac_fail(error, error_size,
                           "invalid partition metadata for '%s'", module->id);
        partition_functions += partition->functions;
        partition_instructions += partition->instructions;
        for (j = 0; j < i; j++)
            if (strcmp(partition->unit, report->partitions[j].unit) == 0)
                return ac_fail(error, error_size,
                               "duplicate partition unit for '%s'", module->id);
    }
    if (!report->partitions_unreported &&
        (partition_functions != report->functions_emitted ||
         partition_instructions != report->instructions_total))
        return ac_fail(error, error_size,
                       "partition totals do not balance for '%s'", module->id);
    expected_production =
        report->instructions_trapped == 0 &&
        report->traps_classified == 0 &&
        report->traps_unclassified == 0 &&
        report->traps_other == 0 &&
        report->body_cuts == 0 &&
        report->partitions_unreported == 0;
    if (report->production_ready != expected_production)
        return ac_fail(error, error_size,
                       "productionReady is inconsistent for '%s'", module->id);
    return 0;
}

static int ac_validate_module(ac_module *module,
                              char *error, size_t error_size) {
    size_t i, j, matches;
    uint32_t functions = 0, variables = 0, tls_variables = 0;
    qsort(module->load_ranges, module->load_range_count,
          sizeof(*module->load_ranges), ac_range_cmp);
    qsort(module->exec_ranges, module->exec_range_count,
          sizeof(*module->exec_ranges), ac_range_cmp);
    qsort(module->function_imports, module->function_import_count,
          sizeof(*module->function_imports), ac_import_cmp);
    qsort(module->exports, module->export_count,
          sizeof(*module->exports), ac_export_cmp);
    qsort(module->variable_imports, module->variable_import_count,
          sizeof(*module->variable_imports), ac_variable_cmp);
    for (i = 0; i < module->variable_import_count; i++)
        qsort(module->variable_imports[i].sites,
              module->variable_imports[i].site_count,
              sizeof(*module->variable_imports[i].sites), ac_site_cmp);
    for (i = 1; i < module->load_range_count; i++)
        if (module->load_ranges[i - 1].end > module->load_ranges[i].begin)
            return ac_fail(error, error_size,
                           "module '%s' has overlapping load ranges", module->id);
    for (i = 1; i < module->exec_range_count; i++)
        if (module->exec_ranges[i - 1].end > module->exec_ranges[i].begin)
            return ac_fail(error, error_size,
                           "module '%s' has overlapping executable ranges",
                           module->id);
    for (i = 0; i < module->exec_range_count; i++) {
        int contained = 0;
        for (j = 0; j < module->load_range_count; j++)
            if (module->exec_ranges[i].begin >= module->load_ranges[j].begin &&
                module->exec_ranges[i].end <= module->load_ranges[j].end)
                contained = 1;
        if (!contained)
            return ac_fail(error, error_size,
                           "module '%s' has executable range outside load ranges",
                           module->id);
    }
    for (i = 0; i < module->dispatch_count; i++) {
        const ac_dispatch *dispatch = &module->dispatch[i];
        if (dispatch->kind == AC_DISPATCH_FUNCTION) {
            char expected[128];
            (void)snprintf(expected, sizeof(expected),
                           "vita_func_%s_%08X", module->id, dispatch->key);
            if (strcmp(dispatch->symbol, expected) != 0)
                return ac_fail(error, error_size,
                               "dispatch symbol is not collision-safe in module '%s'",
                               module->id);
        } else if (strncmp(dispatch->symbol, "vita_hle_", 9) != 0) {
            return ac_fail(error, error_size,
                           "function-import symbol is invalid in module '%s'",
                           module->id);
        }
        if (!ac_in_ranges(dispatch->key & ~1u, module->exec_ranges,
                          module->exec_range_count))
            return ac_fail(error, error_size,
                           "dispatch key 0x%08X is outside module '%s'",
                           dispatch->key, module->id);
        for (j = 0; j < i; j++)
            if (dispatch->key == module->dispatch[j].key)
                return ac_fail(error, error_size,
                               "duplicate dispatch key 0x%08X in module '%s'",
                               dispatch->key, module->id);
    }
    for (i = 0; i < module->function_import_count; i++) {
        const ac_function_import *import = &module->function_imports[i];
        const ac_dispatch *dispatch =
            ac_find_dispatch(module, import->key, &matches);
        char expected_target[160];
        (void)snprintf(expected_target, sizeof(expected_target),
                       "vita_import_target_%s_%08X", module->id,
                       import->target_site);
        if (matches != 1 || dispatch->kind != AC_DISPATCH_IMPORT ||
            dispatch->library_nid != import->library_nid ||
            dispatch->function_nid != import->function_nid ||
            strcmp(dispatch->symbol, import->symbol) != 0 ||
            import->key != (import->stub_address | 1u) ||
            strcmp(import->target_slot, expected_target) != 0 ||
            (import->target_site & 3u) != 0u ||
            !ac_in_ranges(import->target_site, module->load_ranges,
                          module->load_range_count) ||
            (import->strict_unsupported && !import->name))
            return ac_fail(error, error_size,
                           "function-import metadata mismatch in module '%s'",
                           module->id);
        for (j = 0; j < i; j++)
            if (import->key == module->function_imports[j].key ||
                import->target_site ==
                    module->function_imports[j].target_site ||
                strcmp(import->target_slot,
                       module->function_imports[j].target_slot) == 0)
                return ac_fail(error, error_size,
                               "duplicate function import in module '%s'",
                               module->id);
    }
    for (i = 0; i < module->dispatch_count; i++) {
        if (module->dispatch[i].kind == AC_DISPATCH_IMPORT) {
            size_t found = 0;
            for (j = 0; j < module->function_import_count; j++)
                if (module->function_imports[j].key ==
                    module->dispatch[i].key)
                    found++;
            if (found != 1)
                return ac_fail(error, error_size,
                               "dispatch import has no unique metadata in module '%s'",
                               module->id);
        }
    }
    for (i = 0; i < module->export_count; i++) {
        const ac_export *export_item = &module->exports[i];
        if (export_item->kind == AC_EXPORT_FUNCTION) {
            const ac_dispatch *dispatch =
                ac_find_dispatch(module, export_item->dispatch_key, &matches);
            functions++;
            if (matches != 1 || dispatch->kind != AC_DISPATCH_FUNCTION ||
                export_item->address != (export_item->dispatch_key & ~1u))
                return ac_fail(error, error_size,
                               "function export does not resolve in module '%s'",
                               module->id);
        } else if (export_item->kind == AC_EXPORT_VARIABLE) {
            variables++;
            if (!ac_in_ranges(export_item->address, module->load_ranges,
                              module->load_range_count))
                return ac_fail(error, error_size,
                               "variable export is outside module '%s'",
                               module->id);
        } else {
            tls_variables++;
            if (!ac_in_ranges(export_item->address, module->load_ranges,
                              module->load_range_count))
                return ac_fail(error, error_size,
                               "TLS export is outside module '%s'",
                               module->id);
        }
        for (j = 0; j < i; j++)
            if (export_item->library_nid == module->exports[j].library_nid &&
                export_item->nid == module->exports[j].nid)
                return ac_fail(error, error_size,
                               "ambiguous export in module '%s'", module->id);
    }
    if (functions != module->function_export_count ||
        variables != module->variable_export_count ||
        tls_variables != module->tls_variable_export_count ||
        (uint64_t)functions + variables + tls_variables != module->export_count)
        return ac_fail(error, error_size,
                       "export counts do not balance for module '%s'",
                       module->id);
    if (module->entry_present &&
        (module->entry_dispatch_key !=
             vita_module_start_dispatch_key(module->entry_raw_address) ||
         !ac_in_ranges(module->entry_raw_address & ~1u,
                       module->exec_ranges, module->exec_range_count)))
        return ac_fail(error, error_size,
                       "entry address/dispatch mode mismatch for '%s'",
                       module->id);
    if (module->entry_resolved) {
        const ac_dispatch *dispatch =
            ac_find_dispatch(module, module->entry_dispatch_key, &matches);
        if (!module->entry_present || !module->entry_function ||
            matches != 1 || dispatch->kind != AC_DISPATCH_FUNCTION ||
            strcmp(dispatch->symbol, module->entry_function) != 0)
            return ac_fail(error, error_size,
                           "resolved entry metadata is inconsistent for '%s'",
                           module->id);
    } else if (module->entry_function) {
        return ac_fail(error, error_size,
                       "unresolved entry has a function in module '%s'",
                       module->id);
    }
    if (!module->entry_present &&
        (module->entry_raw_address != 0 ||
         module->entry_dispatch_key != 0 || module->entry_resolved))
        return ac_fail(error, error_size,
                       "absent entry metadata is inconsistent for '%s'",
                       module->id);
    if (module->tls_present) {
        uint64_t tls_end = (uint64_t)module->tls_address +
                           module->tls_file_size;
        int contained = module->tls_file_size == 0;
        for (i = 0; i < module->load_range_count; i++)
            if (module->tls_address >= module->load_ranges[i].begin &&
                tls_end <= module->load_ranges[i].end)
                contained = 1;
        if (!module->tls_resolved || !module->tls_memory_size ||
            !module->tls_alignment ||
            (module->tls_alignment & (module->tls_alignment - 1u)) != 0 ||
            module->tls_file_size > module->tls_memory_size ||
            tls_end > UINT32_MAX || !contained)
            return ac_fail(error, error_size,
                           "TLS metadata is inconsistent for '%s'", module->id);
    } else if (module->tls_resolved || module->tls_address ||
               module->tls_file_size || module->tls_memory_size ||
               module->tls_alignment) {
        return ac_fail(error, error_size,
                       "absent TLS metadata is inconsistent for '%s'",
                       module->id);
    }
    for (i = 0; i < module->variable_import_count; i++) {
        const ac_variable_import *import = &module->variable_imports[i];
        char expected[128];
        (void)snprintf(expected, sizeof(expected),
                       "vita_var_%s_%08X_%08X", module->id,
                       import->library_nid, import->variable_nid);
        if (strcmp(import->symbol, expected) != 0)
            return ac_fail(error, error_size,
                           "variable-import symbol is not collision-safe in module '%s'",
                           module->id);
        for (j = 0; j < import->site_count; j++) {
            const ac_variable_site *site = &import->sites[j];
            if ((site->relocation_code != 2u &&
                 site->relocation_code != 0x2Fu &&
                 site->relocation_code != 0x30u) ||
                !ac_in_ranges(site->address, module->exec_ranges,
                              module->exec_range_count))
                return ac_fail(error, error_size,
                               "invalid variable-import site in module '%s'",
                               module->id);
            for (size_t prior = 0; prior < j; prior++)
                if (site->address == import->sites[prior].address &&
                    site->relocation_code ==
                        import->sites[prior].relocation_code)
                    return ac_fail(error, error_size,
                                   "duplicate variable-import site in module '%s'",
                                   module->id);
        }
        for (j = 0; j < i; j++)
            if (import->library_nid ==
                    module->variable_imports[j].library_nid &&
                import->variable_nid ==
                    module->variable_imports[j].variable_nid)
                return ac_fail(error, error_size,
                               "duplicate variable import in module '%s'",
                               module->id);
    }
    for (i = 0; i < module->report.partition_count; i++) {
        const ac_partition *partition = &module->report.partitions[i];
        if (!ac_in_ranges(partition->first_key & ~1u, module->exec_ranges,
                          module->exec_range_count) ||
            !ac_in_ranges(partition->last_key & ~1u, module->exec_ranges,
                          module->exec_range_count))
            return ac_fail(error, error_size,
                           "partition keys are outside module '%s'",
                           module->id);
    }
    return ac_validate_report(module, error, error_size);
}

static int ac_overlap(const ac_range *a, const ac_range *b,
                      uint32_t *begin, uint32_t *end) {
    uint32_t overlap_begin = a->begin > b->begin ? a->begin : b->begin;
    uint32_t overlap_end = a->end < b->end ? a->end : b->end;
    if (overlap_begin >= overlap_end) return 0;
    *begin = overlap_begin;
    *end = overlap_end;
    return 1;
}

static void ac_json_string(FILE *file, const char *text) {
    fputc('"', file);
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c == '"' || c == '\\') fprintf(file, "\\%c", c);
        else if (c == '\b') fputs("\\b", file);
        else if (c == '\f') fputs("\\f", file);
        else if (c == '\n') fputs("\\n", file);
        else if (c == '\r') fputs("\\r", file);
        else if (c == '\t') fputs("\\t", file);
        else if (c < 0x20u) fprintf(file, "\\u%04X", c);
        else fputc(c, file);
    }
    fputc('"', file);
}

static int ac_artifact_path(char *path, size_t capacity, const char *out_c,
                            const char *suffix) {
    char *dot, *slash, *backslash;
    int written = snprintf(path, capacity, "%s", out_c);
    size_t used;
    if (written < 0 || (size_t)written >= capacity) return 1;
    dot = strrchr(path, '.');
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if (dot && (!slash || dot > slash) && (!backslash || dot > backslash))
        *dot = '\0';
    used = strlen(path);
    written = snprintf(path + used, capacity - used, "%s", suffix);
    return written < 0 || (size_t)written >= capacity - used;
}

static const char *ac_base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;
    if (slash && slash + 1 > base) base = slash + 1;
    if (backslash && backslash + 1 > base) base = backslash + 1;
    return base;
}

static int ac_write_header(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) return 1;
    fputs("/* Generated by armrecomp compose. Do not edit. */\n\n", file);
    fputs("#ifndef VITARECOMP_GENERATED_IMAGE_REGISTRY_H\n", file);
    fputs("#define VITARECOMP_GENERATED_IMAGE_REGISTRY_H\n\n", file);
    fputs("#include \"vitarecomp/generated_module.h\"\n\n", file);
    fputs("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n", file);
    fputs("extern const vita_generated_module_descriptor *const\n", file);
    fputs("    vita_generated_image_modules[];\n", file);
    fputs("extern const uint32_t vita_generated_image_module_count;\n", file);
    fputs("extern const vita_generated_tls_layout\n", file);
    fputs("    vita_generated_image_tls_layout;\n", file);
    fputs("extern const vita_dispatch_entry vita_generated_image_dispatch[];\n", file);
    fputs("extern const uint32_t vita_generated_image_dispatch_count;\n\n", file);
    fputs("const vita_generated_module_descriptor *\n", file);
    fputs("vita_generated_image_entry_module(void);\n", file);
    fputs("vita_fn vita_generated_image_entry(void);\n", file);
    fputs("vita_dispatch_status vita_generated_image_init_checked(void);\n", file);
    fputs("void vita_generated_image_init(void);\n\n", file);
    fputs("#ifdef __cplusplus\n}\n#endif\n\n", file);
    fputs("#endif /* VITARECOMP_GENERATED_IMAGE_REGISTRY_H */\n", file);
    return fclose(file) != 0;
}

static int ac_write_registry(const char *out_c, const char *header_path,
                             const ac_module *modules, size_t module_count,
                             const ac_merged_dispatch *dispatch,
                             size_t dispatch_count,
                             const ac_module *entry_module,
                             const ac_dispatch *entry_dispatch,
                             uint32_t tls_module_count,
                             uint32_t tls_data_size,
                             uint32_t tls_data_alignment) {
    FILE *file = fopen(out_c, "wb");
    size_t i, j;
    if (!file) return 1;
    fputs("/* Generated by armrecomp compose. Do not edit. */\n\n", file);
    fprintf(file, "#include \"%s\"\n\n", ac_base_name(header_path));
    for (i = 0; i < module_count; i++)
        fprintf(file, "extern const vita_generated_module_descriptor %s;\n",
                modules[i].descriptor_symbol);
    fputc('\n', file);
    for (i = 0; i < dispatch_count; i++) {
        int seen = 0;
        for (j = 0; j < i; j++)
            if (strcmp(dispatch[i].symbol, dispatch[j].symbol) == 0)
                seen = 1;
        if (!seen) fprintf(file, "extern void %s(void);\n", dispatch[i].symbol);
    }
    fputs("\nconst vita_generated_module_descriptor *const\n", file);
    fputs("vita_generated_image_modules[] = {\n", file);
    for (i = 0; i < module_count; i++)
        fprintf(file, "    &%s%s\n", modules[i].descriptor_symbol,
                i + 1 == module_count ? "" : ",");
    fputs("};\n", file);
    fprintf(file, "const uint32_t vita_generated_image_module_count = %uu;\n\n",
            (unsigned)module_count);
    if (tls_module_count) {
        fputs("static const vita_generated_tls_layout_entry\n", file);
        fputs("vita_generated_image_tls_entries[] = {\n", file);
        for (i = 0; i < module_count; i++) {
            if (!modules[i].tls_present) continue;
            fprintf(file,
                    "    { &%s, %uu, %uu, %uu, %uu }%s\n",
                    modules[i].descriptor_symbol, (unsigned)i,
                    modules[i].tls_order, modules[i].tls_offset,
                    modules[i].tls_alignment,
                    modules[i].tls_order + 1u == tls_module_count
                        ? "" : ",");
        }
        fputs("};\n", file);
    }
    fputs("const vita_generated_tls_layout\n", file);
    fputs("vita_generated_image_tls_layout = {\n", file);
    fputs("    VITA_GENERATED_MODULE_ABI_VERSION,\n", file);
    fputs("    (uint32_t)sizeof(vita_generated_tls_layout),\n", file);
    fprintf(file, "    %s,\n",
            tls_module_count ? "vita_generated_image_tls_entries" : "0");
    fprintf(file, "    %uu,\n", (unsigned)module_count);
    fprintf(file, "    %uu,\n", tls_module_count);
    fprintf(file, "    %uu,\n", tls_data_size);
    fprintf(file, "    %uu\n", tls_data_alignment);
    fputs("};\n\n", file);
    fputs("const vita_dispatch_entry vita_generated_image_dispatch[] = {\n",
          file);
    for (i = 0; i < dispatch_count; i++)
        fprintf(file, "    { 0x%08Xu, %s }%s\n", dispatch[i].key,
                dispatch[i].symbol, i + 1 == dispatch_count ? "" : ",");
    fputs("};\n", file);
    fprintf(file,
            "const uint32_t vita_generated_image_dispatch_count = %uu;\n\n",
            (unsigned)dispatch_count);
    fputs("const vita_generated_module_descriptor *\n", file);
    fputs("vita_generated_image_entry_module(void) {\n", file);
    fprintf(file, "    return &%s;\n", entry_module->descriptor_symbol);
    fputs("}\n\n", file);
    fputs("vita_fn vita_generated_image_entry(void) {\n", file);
    fprintf(file, "    return %s;\n", entry_dispatch->symbol);
    fputs("}\n\n", file);
    fputs("vita_dispatch_status vita_generated_image_init_checked(void) {\n",
          file);
    fputs("    return vita_dispatch_init_checked(\n", file);
    fputs("        vita_generated_image_dispatch,\n", file);
    fputs("        vita_generated_image_dispatch_count);\n", file);
    fputs("}\n\n", file);
    fputs("void vita_generated_image_init(void) {\n", file);
    fputs("    (void)vita_generated_image_init_checked();\n", file);
    fputs("}\n", file);
    return fclose(file) != 0;
}

static int ac_write_json(const char *path, const ac_module *modules,
                         size_t module_count,
                         const ac_module *entry_module,
                         const ac_dispatch *entry_dispatch,
                         int structural_ready, int entry_eligible,
                         int production_ready, int function_entry, int pc,
                         const ac_totals *totals,
                         const ac_merged_trap *traps, size_t trap_count,
                         uint32_t tls_module_count,
                         uint32_t tls_data_size,
                         uint32_t tls_data_alignment) {
    FILE *file = fopen(path, "wb");
    size_t i;
    if (!file) return 1;
    fputs("{\n", file);
    fprintf(file, "  \"schemaVersion\": %u,\n", AC_COMPOSE_SCHEMA_VERSION);
    fputs("  \"generator\": \"armrecomp-compose\",\n", file);
    fputs("  \"entryModuleId\": ", file);
    ac_json_string(file, entry_module->id);
    fputs(",\n", file);
    fprintf(file, "  \"moduleCount\": %u,\n", (unsigned)module_count);
    fprintf(file, "  \"strictUnsupported\": true,\n");
    fprintf(file, "  \"structuralReady\": %s,\n",
            structural_ready ? "true" : "false");
    fprintf(file, "  \"entryEligible\": %s,\n",
            entry_eligible ? "true" : "false");
    fprintf(file, "  \"productionReady\": %s,\n",
            production_ready ? "true" : "false");
    fprintf(file,
            "  \"instrumentation\": {\"functionEntry\": %s, \"pc\": %s},\n",
            function_entry ? "true" : "false", pc ? "true" : "false");
    fputs("  \"entry\": {\"moduleId\": ", file);
    ac_json_string(file, entry_module->id);
    fprintf(file,
            ", \"rawAddress\": \"0x%08X\", "
            "\"dispatchKey\": \"0x%08X\", \"symbol\": ",
            entry_module->entry_raw_address,
            entry_dispatch->key);
    ac_json_string(file, entry_dispatch->symbol);
    fputs(", \"matches\": 1},\n", file);
    fprintf(file,
            "  \"tlsLayout\": {\"moduleCount\": %u, "
            "\"dataSize\": %u, \"dataAlignment\": %u, \"items\": [\n",
            tls_module_count, tls_data_size, tls_data_alignment);
    {
        uint32_t written = 0;
        for (i = 0; i < module_count; i++) {
            const ac_module *module = &modules[i];
            if (!module->tls_present) continue;
            fputs("    {\"moduleId\": ", file);
            ac_json_string(file, module->id);
            fprintf(file,
                    ", \"descriptorIndex\": %u, \"order\": %u, "
                    "\"offset\": %u, \"alignment\": %u}%s\n",
                    (unsigned)i, module->tls_order, module->tls_offset,
                    module->tls_alignment,
                    ++written == tls_module_count ? "" : ",");
        }
    }
    fputs("  ]},\n", file);
    fputs("  \"totals\": {\n", file);
    fprintf(file,
            "    \"ranges\": {\"load\": %" PRIu64
            ", \"executable\": %" PRIu64 "},\n",
            totals->load_ranges, totals->exec_ranges);
    fprintf(file, "    \"dispatchEntries\": %" PRIu64 ",\n",
            totals->dispatch_entries);
    fprintf(file,
            "    \"functions\": {\"emitted\": %" PRIu64
            ", \"discovered\": %" PRIu64 ", \"dropped\": %" PRIu64
            ", \"promotedWeakRoots\": %" PRIu64 "},\n",
            totals->functions_emitted, totals->functions_discovered,
            totals->functions_dropped, totals->functions_promoted);
    fprintf(file,
            "    \"instructions\": {\"total\": %" PRIu64
            ", \"translated\": %" PRIu64 ", \"trapped\": %" PRIu64
            ", \"bodyCuts\": %" PRIu64 "},\n",
            totals->instructions_total, totals->instructions_translated,
            totals->instructions_trapped, totals->body_cuts);
    fprintf(file,
            "    \"traps\": {\"total\": %" PRIu64
            ", \"classified\": %" PRIu64 ", \"unclassified\": %" PRIu64
            ", \"other\": %" PRIu64 ", \"kinds\": [\n",
            totals->traps_total, totals->traps_classified,
            totals->traps_unclassified, totals->traps_other);
    for (i = 0; i < trap_count; i++) {
        fputs("      {\"kind\": ", file);
        ac_json_string(file, traps[i].kind);
        fprintf(file, ", \"count\": %" PRIu64 "}%s\n", traps[i].count,
                i + 1 == trap_count ? "" : ",");
    }
    fputs("    ]},\n", file);
    fprintf(file,
            "    \"partitions\": {\"reported\": %" PRIu64
            ", \"unreported\": %" PRIu64 "},\n",
            totals->partitions_reported, totals->partitions_unreported);
    fprintf(file,
            "    \"metadata\": {\"tlsModules\": %" PRIu64
            ", \"functionImports\": %" PRIu64
            ", \"functionExports\": %" PRIu64
            ", \"variableExports\": %" PRIu64
            ", \"tlsVariableExports\": %" PRIu64
            ", \"variableImports\": %" PRIu64
            ", \"variableSites\": %" PRIu64 "}\n",
            totals->tls_modules, totals->function_imports,
            totals->function_exports, totals->variable_exports,
            totals->tls_variable_exports, totals->variable_imports,
            totals->variable_sites);
    fputs("  },\n", file);
    fputs("  \"modules\": [\n", file);
    for (i = 0; i < module_count; i++) {
        const ac_module *module = &modules[i];
        uint64_t sites = 0;
        size_t r, v, s;
        for (v = 0; v < module->variable_import_count; v++)
            sites += module->variable_imports[v].site_count;
        fputs("    {\"id\": ", file);
        ac_json_string(file, module->id);
        fputs(", \"name\": ", file);
        ac_json_string(file, module->name);
        fputs(", \"descriptorSymbol\": ", file);
        ac_json_string(file, module->descriptor_symbol);
        fputs(", \"ranges\": {\"load\": [", file);
        for (r = 0; r < module->load_range_count; r++)
            fprintf(file,
                    "%s{\"begin\": \"0x%08X\", "
                    "\"endExclusive\": \"0x%08X\"}",
                    r ? ", " : "", module->load_ranges[r].begin,
                    module->load_ranges[r].end);
        fputs("], \"executable\": [", file);
        for (r = 0; r < module->exec_range_count; r++)
            fprintf(file,
                    "%s{\"begin\": \"0x%08X\", "
                    "\"endExclusive\": \"0x%08X\"}",
                    r ? ", " : "", module->exec_ranges[r].begin,
                    module->exec_ranges[r].end);
        fprintf(file,
                "]}, \"dispatchEntries\": %u, "
                "\"entry\": {\"present\": %s, "
                "\"rawAddress\": \"0x%08X\", "
                "\"dispatchKey\": \"0x%08X\", \"function\": ",
                (unsigned)module->dispatch_count,
                module->entry_present ? "true" : "false",
                module->entry_raw_address,
                module->entry_dispatch_key);
        if (module->entry_function)
            ac_json_string(file, module->entry_function);
        else
            fputs("null", file);
        fprintf(file,
                ", \"resolved\": %s}, "
                "\"tls\": {\"present\": %s, \"resolved\": %s, "
                "\"address\": \"0x%08X\", \"fileSize\": %u, "
                "\"memorySize\": %u, \"alignment\": %u, "
                "\"order\": ",
                module->entry_resolved ? "true" : "false",
                module->tls_present ? "true" : "false",
                module->tls_resolved ? "true" : "false",
                module->tls_address, module->tls_file_size,
                module->tls_memory_size, module->tls_alignment);
        if (module->tls_present)
            fprintf(file, "%u, \"offset\": %u}, ",
                    module->tls_order, module->tls_offset);
        else
            fputs("null, \"offset\": null}, ", file);

        fprintf(file, "\"functionImports\": {\"count\": %u, \"items\": [",
                (unsigned)module->function_import_count);
        for (v = 0; v < module->function_import_count; v++) {
            const ac_function_import *import =
                &module->function_imports[v];
            fprintf(file,
                    "%s{\"key\": \"0x%08X\", "
                    "\"stubAddress\": \"0x%08X\", "
                    "\"targetSite\": \"0x%08X\", "
                    "\"libraryNid\": \"0x%08X\", "
                    "\"functionNid\": \"0x%08X\", \"name\": ",
                    v ? ", " : "", import->key, import->stub_address,
                    import->target_site, import->library_nid,
                    import->function_nid);
            if (import->name) ac_json_string(file, import->name);
            else fputs("null", file);
            fputs(", \"symbol\": ", file);
            ac_json_string(file, import->symbol);
            fputs(", \"targetSlot\": ", file);
            ac_json_string(file, import->target_slot);
            fprintf(file, ", \"strictUnsupported\": %s}",
                    import->strict_unsupported ? "true" : "false");
        }
        fputs("]}, ", file);

        fprintf(file,
                "\"exports\": {\"count\": %u, \"functions\": %u, "
                "\"variables\": %u, \"tlsVariables\": %u, \"items\": [",
                (unsigned)module->export_count,
                module->function_export_count,
                module->variable_export_count,
                module->tls_variable_export_count);
        for (v = 0; v < module->export_count; v++) {
            const ac_export *export_item = &module->exports[v];
            const char *kind = export_item->kind == AC_EXPORT_FUNCTION
                ? "function" : export_item->kind == AC_EXPORT_VARIABLE
                ? "variable" : "tlsVariable";
            fprintf(file,
                    "%s{\"kind\": \"%s\", "
                    "\"libraryNid\": \"0x%08X\", \"nid\": \"0x%08X\", "
                    "\"address\": \"0x%08X\", \"dispatchKey\": ",
                    v ? ", " : "", kind, export_item->library_nid,
                    export_item->nid, export_item->address);
            if (export_item->has_dispatch_key)
                fprintf(file, "\"0x%08X\"", export_item->dispatch_key);
            else
                fputs("null", file);
            fputc('}', file);
        }
        fputs("]}, ", file);

        fprintf(file,
                "\"variableImports\": {\"count\": %u, "
                "\"siteCount\": %" PRIu64 ", \"items\": [",
                (unsigned)module->variable_import_count, sites);
        for (v = 0; v < module->variable_import_count; v++) {
            const ac_variable_import *import =
                &module->variable_imports[v];
            fprintf(file,
                    "%s{\"libraryNid\": \"0x%08X\", "
                    "\"variableNid\": \"0x%08X\", "
                    "\"stubAddress\": \"0x%08X\", \"name\": ",
                    v ? ", " : "", import->library_nid,
                    import->variable_nid, import->stub_address);
            if (import->name) ac_json_string(file, import->name);
            else fputs("null", file);
            fputs(", \"symbol\": ", file);
            ac_json_string(file, import->symbol);
            fputs(", \"sites\": [", file);
            for (s = 0; s < import->site_count; s++)
                fprintf(file,
                        "%s{\"address\": \"0x%08X\", \"addend\": %u, "
                        "\"relocationCode\": %u}",
                        s ? ", " : "", import->sites[s].address,
                        import->sites[s].addend,
                        import->sites[s].relocation_code);
            fputs("]}", file);
        }
        fputs("]}, ", file);

        fprintf(file,
                "\"report\": {\"structuralReady\": %s, "
                "\"productionReady\": %s, "
                "\"instrumentation\": {\"functionEntry\": %s, "
                "\"pc\": %s}, "
                "\"functions\": {\"emitted\": %u, \"discovered\": %u, "
                "\"dropped\": %u, \"promotedWeakRoots\": %u}, "
                "\"instructions\": {\"total\": %u, \"translated\": %u, "
                "\"trapped\": %u, \"bodyCuts\": %u}, "
                "\"traps\": {\"total\": %" PRIu64
                ", \"classified\": %" PRIu64 ", \"unclassified\": %u, "
                "\"other\": %u, \"kinds\": %u}, "
                "\"partitions\": {\"reported\": %u, "
                "\"unreported\": %u}}}%s\n",
                module->report.structural_ready ? "true" : "false",
                module->report.production_ready ? "true" : "false",
                module->report.instrumentation_function_entry
                    ? "true" : "false",
                module->report.instrumentation_pc ? "true" : "false",
                module->report.functions_emitted,
                module->report.functions_discovered,
                module->report.functions_dropped,
                module->report.functions_promoted,
                module->report.instructions_total,
                module->report.instructions_translated,
                module->report.instructions_trapped,
                module->report.body_cuts,
                module->report.traps_total,
                module->report.traps_classified,
                module->report.traps_unclassified,
                module->report.traps_other,
                (unsigned)module->report.trap_kind_count,
                (unsigned)module->report.partition_count,
                module->report.partitions_unreported,
                i + 1 == module_count ? "" : ",");
    }
    fputs("  ]\n", file);
    fputs("}\n", file);
    return fclose(file) != 0;
}

static int ac_write_text(const char *path, const ac_module *modules,
                         size_t module_count,
                         const ac_module *entry_module,
                         const ac_dispatch *entry_dispatch,
                         int structural_ready, int entry_eligible,
                         int production_ready, int function_entry, int pc,
                         const ac_totals *totals,
                         const ac_merged_trap *traps, size_t trap_count,
                         uint32_t tls_module_count,
                         uint32_t tls_data_size,
                         uint32_t tls_data_alignment) {
    FILE *file = fopen(path, "wb");
    size_t i;
    if (!file) return 1;
    fprintf(file, "armrecomp composed generation report v%u\n",
            AC_COMPOSE_SCHEMA_VERSION);
    fprintf(file, "entry.module_id=%s\n", entry_module->id);
    fprintf(file, "entry.raw_address=0x%08X\n",
            entry_module->entry_raw_address);
    fprintf(file, "entry.dispatch_key=0x%08X\n", entry_dispatch->key);
    fprintf(file, "entry.symbol=%s\n", entry_dispatch->symbol);
    fputs("entry.matches=1\n", file);
    fprintf(file, "modules.count=%u\n", (unsigned)module_count);
    fputs("strict_unsupported=true\n", file);
    fprintf(file, "structural_ready=%s\n",
            structural_ready ? "true" : "false");
    fprintf(file, "entry_eligible=%s\n", entry_eligible ? "true" : "false");
    fprintf(file, "production_ready=%s\n",
            production_ready ? "true" : "false");
    fprintf(file, "instrumentation.function_entry=%s\n",
            function_entry ? "true" : "false");
    fprintf(file, "instrumentation.pc=%s\n", pc ? "true" : "false");
    fprintf(file, "ranges.load=%" PRIu64 "\n", totals->load_ranges);
    fprintf(file, "ranges.executable=%" PRIu64 "\n", totals->exec_ranges);
    fprintf(file, "dispatch.entries=%" PRIu64 "\n",
            totals->dispatch_entries);
    fprintf(file, "functions.emitted=%" PRIu64 "\n",
            totals->functions_emitted);
    fprintf(file, "functions.discovered=%" PRIu64 "\n",
            totals->functions_discovered);
    fprintf(file, "functions.dropped=%" PRIu64 "\n",
            totals->functions_dropped);
    fprintf(file, "functions.promoted_weak_roots=%" PRIu64 "\n",
            totals->functions_promoted);
    fprintf(file, "instructions.total=%" PRIu64 "\n",
            totals->instructions_total);
    fprintf(file, "instructions.translated=%" PRIu64 "\n",
            totals->instructions_translated);
    fprintf(file, "instructions.trapped=%" PRIu64 "\n",
            totals->instructions_trapped);
    fprintf(file, "instructions.body_cuts=%" PRIu64 "\n",
            totals->body_cuts);
    fprintf(file, "traps.total=%" PRIu64 "\n", totals->traps_total);
    fprintf(file, "traps.classified=%" PRIu64 "\n",
            totals->traps_classified);
    fprintf(file, "traps.unclassified=%" PRIu64 "\n",
            totals->traps_unclassified);
    fprintf(file, "traps.other=%" PRIu64 "\n", totals->traps_other);
    for (i = 0; i < trap_count; i++) {
        fprintf(file, "trap.%u.kind=%s\n", (unsigned)i, traps[i].kind);
        fprintf(file, "trap.%u.count=%" PRIu64 "\n",
                (unsigned)i, traps[i].count);
    }
    fprintf(file, "partitions.reported=%" PRIu64 "\n",
            totals->partitions_reported);
    fprintf(file, "partitions.unreported=%" PRIu64 "\n",
            totals->partitions_unreported);
    fprintf(file, "metadata.tls_modules=%" PRIu64 "\n",
            totals->tls_modules);
    fprintf(file, "tls_layout.module_count=%u\n", tls_module_count);
    fprintf(file, "tls_layout.data_size=%u\n", tls_data_size);
    fprintf(file, "tls_layout.data_alignment=%u\n", tls_data_alignment);
    fprintf(file, "metadata.function_imports=%" PRIu64 "\n",
            totals->function_imports);
    fprintf(file, "metadata.function_exports=%" PRIu64 "\n",
            totals->function_exports);
    fprintf(file, "metadata.variable_exports=%" PRIu64 "\n",
            totals->variable_exports);
    fprintf(file, "metadata.tls_variable_exports=%" PRIu64 "\n",
            totals->tls_variable_exports);
    fprintf(file, "metadata.variable_imports=%" PRIu64 "\n",
            totals->variable_imports);
    fprintf(file, "metadata.variable_sites=%" PRIu64 "\n",
            totals->variable_sites);
    for (i = 0; i < module_count; i++) {
        uint64_t sites = 0;
        size_t v;
        for (v = 0; v < modules[i].variable_import_count; v++)
            sites += modules[i].variable_imports[v].site_count;
        fprintf(file, "module.%u.id=%s\n", (unsigned)i, modules[i].id);
        fprintf(file, "module.%u.name=%s\n", (unsigned)i, modules[i].name);
        fprintf(file, "module.%u.descriptor=%s\n", (unsigned)i,
                modules[i].descriptor_symbol);
        fprintf(file, "module.%u.load_begin=0x%08X\n", (unsigned)i,
                modules[i].load_ranges[0].begin);
        fprintf(file, "module.%u.load_ranges=%u\n", (unsigned)i,
                (unsigned)modules[i].load_range_count);
        fprintf(file, "module.%u.executable_ranges=%u\n", (unsigned)i,
                (unsigned)modules[i].exec_range_count);
        fprintf(file, "module.%u.dispatch_entries=%u\n", (unsigned)i,
                (unsigned)modules[i].dispatch_count);
        fprintf(file, "module.%u.entry.present=%s\n", (unsigned)i,
                modules[i].entry_present ? "true" : "false");
        fprintf(file, "module.%u.entry.raw_address=0x%08X\n", (unsigned)i,
                modules[i].entry_raw_address);
        fprintf(file, "module.%u.entry.dispatch_key=0x%08X\n", (unsigned)i,
                modules[i].entry_dispatch_key);
        fprintf(file, "module.%u.entry.resolved=%s\n", (unsigned)i,
                modules[i].entry_resolved ? "true" : "false");
        fprintf(file, "module.%u.tls.present=%s\n", (unsigned)i,
                modules[i].tls_present ? "true" : "false");
        fprintf(file, "module.%u.tls.resolved=%s\n", (unsigned)i,
                modules[i].tls_resolved ? "true" : "false");
        fprintf(file, "module.%u.tls.address=0x%08X\n", (unsigned)i,
                modules[i].tls_address);
        fprintf(file, "module.%u.tls.file_size=%u\n", (unsigned)i,
                modules[i].tls_file_size);
        fprintf(file, "module.%u.tls.memory_size=%u\n", (unsigned)i,
                modules[i].tls_memory_size);
        fprintf(file, "module.%u.tls.alignment=%u\n", (unsigned)i,
                modules[i].tls_alignment);
        if (modules[i].tls_present) {
            fprintf(file, "module.%u.tls.order=%u\n", (unsigned)i,
                    modules[i].tls_order);
            fprintf(file, "module.%u.tls.offset=%u\n", (unsigned)i,
                    modules[i].tls_offset);
        }
        fprintf(file, "module.%u.function_imports=%u\n", (unsigned)i,
                (unsigned)modules[i].function_import_count);
        fprintf(file, "module.%u.function_exports=%u\n", (unsigned)i,
                modules[i].function_export_count);
        fprintf(file, "module.%u.variable_exports=%u\n", (unsigned)i,
                modules[i].variable_export_count);
        fprintf(file, "module.%u.tls_variable_exports=%u\n", (unsigned)i,
                modules[i].tls_variable_export_count);
        fprintf(file, "module.%u.variable_imports=%u\n", (unsigned)i,
                (unsigned)modules[i].variable_import_count);
        fprintf(file, "module.%u.variable_sites=%" PRIu64 "\n",
                (unsigned)i, sites);
        fprintf(file, "module.%u.instrumentation.function_entry=%s\n",
                (unsigned)i,
                modules[i].report.instrumentation_function_entry
                    ? "true" : "false");
        fprintf(file, "module.%u.instrumentation.pc=%s\n", (unsigned)i,
                modules[i].report.instrumentation_pc ? "true" : "false");
        fprintf(file, "module.%u.structural_ready=%s\n", (unsigned)i,
                modules[i].report.structural_ready ? "true" : "false");
        fprintf(file, "module.%u.production_ready=%s\n", (unsigned)i,
                modules[i].report.production_ready ? "true" : "false");
        fprintf(file, "module.%u.body_cuts=%u\n", (unsigned)i,
                modules[i].report.body_cuts);
        fprintf(file, "module.%u.traps.unclassified=%u\n", (unsigned)i,
                modules[i].report.traps_unclassified);
        fprintf(file, "module.%u.partitions.unreported=%u\n", (unsigned)i,
                modules[i].report.partitions_unreported);
    }
    return fclose(file) != 0;
}

int ac_compose(const char *out_c, const char *entry_module_id,
               const char *const *inputs, size_t input_count,
               char *error, size_t error_size) {
    ac_module *modules = NULL;
    ac_report *reports = NULL;
    ac_merged_dispatch *dispatch = NULL;
    ac_merged_trap *traps = NULL;
    ac_totals totals;
    size_t module_count = 0, report_count = 0;
    size_t dispatch_count = 0, trap_count = 0;
    size_t i, j, k;
    ac_module *entry_module = NULL;
    const ac_dispatch *entry_dispatch = NULL;
    size_t entry_matches = 0;
    uint32_t tls_module_count = 0;
    uint32_t tls_data_size = 0;
    uint32_t tls_data_alignment = 16u;
    int structural_ready = 1;
    int production_ready = 1;
    int function_entry = 1;
    int pc = 1;
    int result = 1;
    char header_path[1200], json_path[1200], text_path[1200];

    if (error && error_size) error[0] = '\0';
    memset(&totals, 0, sizeof(totals));
    if (!out_c || !*out_c || !ac_module_id_valid(entry_module_id) ||
        !inputs || input_count < 2 || (input_count & 1u))
        return ac_fail(error, error_size,
                       "compose requires an output, valid entry module ID, and sidecar/report pairs");

    modules = (ac_module *)calloc(input_count / 2, sizeof(*modules));
    reports = (ac_report *)calloc(input_count / 2, sizeof(*reports));
    if (!modules || !reports) {
        ac_fail(error, error_size, "out of memory");
        goto cleanup;
    }
    for (i = 0; i < input_count; i++) {
        ac_file file;
        jvalue *root;
        const jvalue *schema, *abi;
        if (ac_read_file(inputs[i], &file, error, error_size)) goto cleanup;
        root = jp_document(inputs[i], &file, error, error_size);
        free(file.text);
        if (!root) goto cleanup;
        schema = jv_member(root, "schemaVersion");
        abi = jv_member(root, "abiVersion");
        if (abi) {
            if (module_count >= input_count / 2) {
                jv_free(root);
                goto cleanup;
            }
            if (ac_parse_sidecar(inputs[i], root, &modules[module_count],
                                 error, error_size)) {
                ac_module_free(&modules[module_count]);
                jv_free(root);
                goto cleanup;
            }
            module_count++;
        } else if (schema && schema->kind == JV_UINT &&
                   schema->as.uint_value ==
                       VITA_GENERATED_REPORT_SCHEMA_VERSION) {
            if (report_count >= input_count / 2) {
                jv_free(root);
                goto cleanup;
            }
            if (ac_parse_report(inputs[i], root, &reports[report_count],
                                error, error_size)) {
                ac_report_free(&reports[report_count]);
                jv_free(root);
                goto cleanup;
            }
            report_count++;
        } else {
            ac_fail(error, error_size, "%s: unsupported JSON schema", inputs[i]);
            jv_free(root);
            goto cleanup;
        }
        jv_free(root);
    }
    if (!module_count || module_count != report_count ||
        module_count * 2 != input_count) {
        ac_fail(error, error_size,
                "compose requires exactly one sidecar and report per module");
        goto cleanup;
    }

    for (i = 0; i < module_count; i++) {
        size_t matches = 0, match_index = 0;
        for (j = 0; j < report_count; j++) {
            if (reports[j].module_id &&
                strcmp(modules[i].id, reports[j].module_id) == 0) {
                matches++;
                match_index = j;
            }
        }
        if (matches != 1) {
            ac_fail(error, error_size,
                    "module '%s' has %u matching reports",
                    modules[i].id, (unsigned)matches);
            goto cleanup;
        }
        modules[i].report = reports[match_index];
        memset(&reports[match_index], 0, sizeof(reports[match_index]));
        modules[i].has_report = 1;
        if (ac_validate_module(&modules[i], error, error_size)) goto cleanup;
        for (j = 0; j < i; j++)
            if (strcmp(modules[i].id, modules[j].id) == 0 ||
                strcmp(modules[i].descriptor_symbol,
                       modules[j].descriptor_symbol) == 0) {
                ac_fail(error, error_size,
                        "ambiguous module ID or descriptor '%s'",
                        modules[i].id);
                goto cleanup;
            }
    }

    qsort(modules, module_count, sizeof(*modules), ac_module_cmp);
    for (i = 0; i < module_count; i++) {
        ac_module *module = &modules[i];
        module->tls_order = UINT32_MAX;
        module->tls_offset = 0;
        if (!module->tls_present) continue;
        uint32_t offset = 0;
        if (ac_align_up(tls_data_size, module->tls_alignment, &offset) ||
            module->tls_memory_size > UINT32_MAX - offset) {
            ac_fail(error, error_size,
                    "TLS aggregate layout overflows at module '%s'",
                    module->id);
            goto cleanup;
        }
        module->tls_order = tls_module_count++;
        module->tls_offset = offset;
        tls_data_size = offset + module->tls_memory_size;
        if (module->tls_alignment > tls_data_alignment)
            tls_data_alignment = module->tls_alignment;
    }
    for (i = 0; i < module_count; i++) {
        for (j = i + 1; j < module_count; j++) {
            uint32_t begin, end;
            for (k = 0; k < modules[i].exec_range_count; k++) {
                size_t n;
                for (n = 0; n < modules[j].exec_range_count; n++)
                    if (ac_overlap(&modules[i].exec_ranges[k],
                                   &modules[j].exec_ranges[n], &begin, &end)) {
                        ac_fail(error, error_size,
                                "executable-range overlap: %s/%s [0x%08X,0x%08X)",
                                modules[i].id, modules[j].id, begin, end);
                        goto cleanup;
                    }
            }
            for (k = 0; k < modules[i].load_range_count; k++) {
                size_t n;
                for (n = 0; n < modules[j].load_range_count; n++)
                    if (ac_overlap(&modules[i].load_ranges[k],
                                   &modules[j].load_ranges[n], &begin, &end)) {
                        ac_fail(error, error_size,
                                "load-range overlap: %s/%s [0x%08X,0x%08X)",
                                modules[i].id, modules[j].id, begin, end);
                        goto cleanup;
                    }
            }
            for (k = 0; k < modules[i].export_count; k++) {
                size_t n;
                for (n = 0; n < modules[j].export_count; n++) {
                    const ac_export *a = &modules[i].exports[k];
                    const ac_export *b = &modules[j].exports[n];
                    if (a->library_nid == b->library_nid &&
                        a->nid == b->nid) {
                        ac_fail(error, error_size,
                                "ambiguous export 0x%08X:0x%08X in %s/%s",
                                a->library_nid, a->nid,
                                modules[i].id, modules[j].id);
                        goto cleanup;
                    }
                }
            }
        }
        dispatch_count += modules[i].dispatch_count;
        trap_count += modules[i].report.trap_kind_count;
    }
    dispatch = (ac_merged_dispatch *)calloc(dispatch_count, sizeof(*dispatch));
    traps = (ac_merged_trap *)calloc(trap_count ? trap_count : 1,
                                     sizeof(*traps));
    if (!dispatch || !traps) {
        ac_fail(error, error_size, "out of memory");
        goto cleanup;
    }
    dispatch_count = 0;
    trap_count = 0;
    for (i = 0; i < module_count; i++) {
        ac_module *module = &modules[i];
        const ac_report *report = &module->report;
        totals.load_ranges += module->load_range_count;
        totals.exec_ranges += module->exec_range_count;
        totals.dispatch_entries += module->dispatch_count;
        totals.functions_emitted += report->functions_emitted;
        totals.functions_discovered += report->functions_discovered;
        totals.functions_dropped += report->functions_dropped;
        totals.functions_promoted += report->functions_promoted;
        totals.instructions_total += report->instructions_total;
        totals.instructions_translated += report->instructions_translated;
        totals.instructions_trapped += report->instructions_trapped;
        totals.body_cuts += report->body_cuts;
        totals.traps_total += report->traps_total;
        totals.traps_classified += report->traps_classified;
        totals.traps_unclassified += report->traps_unclassified;
        totals.traps_other += report->traps_other;
        totals.partitions_reported += report->partition_count;
        totals.partitions_unreported += report->partitions_unreported;
        totals.tls_modules += module->tls_present ? 1u : 0u;
        totals.function_imports += module->function_import_count;
        totals.function_exports += module->function_export_count;
        totals.variable_exports += module->variable_export_count;
        totals.tls_variable_exports += module->tls_variable_export_count;
        totals.variable_imports += module->variable_import_count;
        for (j = 0; j < module->variable_import_count; j++)
            totals.variable_sites += module->variable_imports[j].site_count;
        for (j = 0; j < module->dispatch_count; j++) {
            dispatch[dispatch_count].key = module->dispatch[j].key;
            dispatch[dispatch_count].symbol = module->dispatch[j].symbol;
            dispatch[dispatch_count].module_id = module->id;
            dispatch_count++;
        }
        for (j = 0; j < report->trap_kind_count; j++) {
            size_t found = trap_count;
            for (k = 0; k < trap_count; k++)
                if (strcmp(traps[k].kind, report->trap_kinds[j].kind) == 0)
                    found = k;
            if (found == trap_count) {
                traps[trap_count].kind = report->trap_kinds[j].kind;
                traps[trap_count].count = report->trap_kinds[j].count;
                trap_count++;
            } else {
                traps[found].count += report->trap_kinds[j].count;
            }
        }
        function_entry = function_entry && report->instrumentation_function_entry;
        pc = pc && report->instrumentation_pc;
        production_ready = production_ready && report->production_ready;
        structural_ready = structural_ready &&
            report->structural_ready && report->body_cuts == 0 &&
            report->partitions_unreported == 0 &&
            report->traps_unclassified == 0 &&
            report->instrumentation_function_entry &&
            report->instrumentation_pc &&
            (!module->entry_present || module->entry_resolved) &&
            (!module->tls_present || module->tls_resolved);
        if (strcmp(module->id, entry_module_id) == 0) {
            if (entry_module) {
                ac_fail(error, error_size,
                        "entry module ID '%s' is ambiguous", entry_module_id);
                goto cleanup;
            }
            entry_module = module;
        }
    }
    qsort(dispatch, dispatch_count, sizeof(*dispatch), ac_dispatch_cmp);
    for (i = 1; i < dispatch_count; i++) {
        if (dispatch[i - 1].key == dispatch[i].key) {
            ac_fail(error, error_size,
                    "duplicate dispatch key 0x%08X (%s/%s)",
                    dispatch[i].key, dispatch[i - 1].module_id,
                    dispatch[i].module_id);
            goto cleanup;
        }
    }
    qsort(traps, trap_count, sizeof(*traps), ac_trap_cmp);
    if (!entry_module || !entry_module->entry_present ||
        !entry_module->entry_resolved) {
        ac_fail(error, error_size,
                "entry module '%s' has no resolved entry key",
                entry_module_id);
        goto cleanup;
    }
    for (i = 0; i < dispatch_count; i++) {
        if (dispatch[i].key == entry_module->entry_dispatch_key) {
            entry_matches++;
            entry_dispatch = ac_find_dispatch(entry_module,
                                              entry_module->entry_dispatch_key,
                                              &j);
        }
    }
    if (entry_matches != 1 || !entry_dispatch ||
        strcmp(entry_dispatch->symbol, entry_module->entry_function) != 0) {
        ac_fail(error, error_size,
                "entry key 0x%08X resolves %u times",
                entry_module->entry_dispatch_key,
                (unsigned)entry_matches);
        goto cleanup;
    }
    if (ac_artifact_path(header_path, sizeof(header_path), out_c, ".h") ||
        ac_artifact_path(json_path, sizeof(json_path), out_c, ".report.json") ||
        ac_artifact_path(text_path, sizeof(text_path), out_c, ".report.txt")) {
        ac_fail(error, error_size, "output path is too long");
        goto cleanup;
    }
    if (ac_write_header(header_path) ||
        ac_write_registry(out_c, header_path, modules, module_count,
                          dispatch, dispatch_count, entry_module,
                          entry_dispatch, tls_module_count,
                          tls_data_size, tls_data_alignment) ||
        ac_write_json(json_path, modules, module_count, entry_module,
                      entry_dispatch, structural_ready, structural_ready,
                      production_ready, function_entry, pc, &totals,
                      traps, trap_count, tls_module_count,
                      tls_data_size, tls_data_alignment) ||
        ac_write_text(text_path, modules, module_count, entry_module,
                      entry_dispatch, structural_ready, structural_ready,
                      production_ready, function_entry, pc, &totals,
                      traps, trap_count, tls_module_count,
                      tls_data_size, tls_data_alignment)) {
        remove(out_c);
        remove(header_path);
        remove(json_path);
        remove(text_path);
        ac_fail(error, error_size, "cannot write composed artifacts");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (modules) {
        for (i = 0; i < module_count; i++) ac_module_free(&modules[i]);
    }
    if (reports) {
        for (i = 0; i < report_count; i++) ac_report_free(&reports[i]);
    }
    free(modules);
    free(reports);
    free(dispatch);
    free(traps);
    return result;
}
