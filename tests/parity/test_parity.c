#include <flashfs/flashfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

#define LINE_MAX_LEN 65536
#define LINES_MAX 4096

typedef struct limits {
    unsigned long long total;
    unsigned long long file;
    unsigned long long entries;
} limits_t;

typedef struct lines {
    char *text[LINES_MAX];
    size_t count;
    int failed;
    int damaged;
} lines_t;

static int verbose;

static void add_line(lines_t *out, const char *text)
{
    size_t len = strlen(text);

    if (out->count == LINES_MAX) {
        out->failed = 1;
        return;
    }
    out->text[out->count] = malloc(len + 1);
    if (out->text[out->count] == NULL)
        abort();
    memcpy(out->text[out->count++], text, len + 1);
}

static void free_lines(lines_t *l)
{
    while (l->count > 0)
        free(l->text[--l->count]);
}

static void hex(char *out, const void *data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    const unsigned char *p = data;
    size_t i;

    for (i = 0; i < len; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 15];
    }
    out[2 * len] = 0;
}

static size_t utf8_sequence(const unsigned char *p, size_t left)
{
    unsigned char lo = 0x80;
    unsigned char hi = 0xbf;
    size_t need;
    size_t i;

    if (p[0] < 0x80)
        return 1;
    if (p[0] >= 0xc2 && p[0] <= 0xdf)
        need = 2;
    else if (p[0] >= 0xe0 && p[0] <= 0xef)
        need = 3;
    else if (p[0] >= 0xf0 && p[0] <= 0xf4)
        need = 4;
    else
        return 0;
    if (p[0] == 0xe0)
        lo = 0xa0;
    else if (p[0] == 0xed)
        hi = 0x9f;
    else if (p[0] == 0xf0)
        lo = 0x90;
    else if (p[0] == 0xf4)
        hi = 0x8f;
    for (i = 1; i < need; i++) {
        if (i >= left || p[i] < lo || p[i] > hi)
            return 0;
        lo = 0x80;
        hi = 0xbf;
    }
    return need;
}

static void python_name_hex(char *out, const void *data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    const unsigned char *p = data;
    unsigned char escaped[4];
    size_t i = 0;
    size_t n;

    *out = 0;
    while (i < len) {
        n = utf8_sequence(p + i, len - i);
        if (n > 0) {
            hex(out, p + i, n);
            out += 2 * n;
            i += n;
            continue;
        }
        escaped[0] = '\\';
        escaped[1] = 'x';
        escaped[2] = (unsigned char)digits[p[i] >> 4];
        escaped[3] = (unsigned char)digits[p[i] & 15];
        hex(out, escaped, 4);
        out += 8;
        i++;
    }
}

static const char *kind(uint32_t type)
{
    switch (type) {
    case MTDF_TYPE_FILE:
        return "file";
    case MTDF_TYPE_DIR:
        return "dir";
    case MTDF_TYPE_LINK:
        return "link";
    case MTDF_TYPE_CHR:
    case MTDF_TYPE_BLK:
        return "device";
    default:
        return "other";
    }
}

static unsigned char *content(mtdf_fs_t *fs, mtdf_ino_t ino, uint64_t size, int *failed)
{
    unsigned char *data = malloc(size ? (size_t)size : 1);
    size_t got;
    uint64_t off = 0;

    if (data == NULL)
        abort();
    while (off < size) {
        if (mtdf_read(fs, ino, off, data + off, (size_t)(size - off), &got) != MTDF_OK || got == 0) {
            if (verbose)
                printf("    read failed: %s\n", mtdf_detail(fs));
            *failed = 1;
            free(data);
            return NULL;
        }
        off += got;
    }
    return data;
}

static void entry_line(mtdf_fs_t *fs, const char *path, size_t path_len, mtdf_ino_t ino, lines_t *out)
{
    char line[LINE_MAX_LEN];
    char path_hex[8 * MTDF_PATH_MAX + 1];
    char digest_hex[65];
    unsigned char digest[32];
    char target[MTDF_PATH_MAX + 1];
    unsigned char *data;
    mtdf_stat_t st;
    size_t len;
    int n;

    if (mtdf_stat(fs, ino, &st) != MTDF_OK) {
        out->failed = 1;
        return;
    }
    python_name_hex(path_hex, path, path_len);
    n = snprintf(line, sizeof(line), "%s %o %u %u %s", kind(st.type), st.mode, st.uid, st.gid, path_hex);
    if (st.type == MTDF_TYPE_FILE) {
        data = content(fs, ino, st.size, &out->failed);
        if (data == NULL)
            return;
        sha256(data, (size_t)st.size, digest);
        free(data);
        hex(digest_hex, digest, 32);
        snprintf(line + n, sizeof(line) - (size_t)n, " %llu %s", (unsigned long long)st.size, digest_hex);
    } else if (st.type == MTDF_TYPE_LINK) {
        if (mtdf_readlink(fs, ino, target, sizeof(target), &len) != MTDF_OK) {
            out->failed = 1;
            return;
        }
        python_name_hex(path_hex, target, len);
        snprintf(line + n, sizeof(line) - (size_t)n, " %s", path_hex);
    }
    add_line(out, line);
}

static void walk(mtdf_fs_t *fs, mtdf_ino_t dir, char *path, size_t len, lines_t *out)
{
    mtdf_dirent_t e;
    uint64_t cookie = 0;
    mtdf_stat_t st;
    size_t n;

    while (mtdf_readdir(fs, dir, &cookie, &e) == MTDF_OK) {
        n = len;
        path[n++] = '/';
        memcpy(path + n, e.name, e.name_len);
        n += e.name_len;
        entry_line(fs, path, n, e.ino, out);
        if (mtdf_stat(fs, e.ino, &st) == MTDF_OK && st.type == MTDF_TYPE_DIR)
            walk(fs, e.ino, path, n, out);
    }
}

static void volume_line(mtdf_fs_t *fs, const mtdf_dirent_t *e, lines_t *out)
{
    char line[LINE_MAX_LEN];
    char name_hex[8 * 256 + 1];
    char digest_hex[65];
    unsigned char digest[32];
    unsigned char *data;
    mtdf_stat_t st;
    uint64_t len;

    if (mtdf_stat(fs, e->ino, &st) != MTDF_OK) {
        out->failed = 1;
        return;
    }
    data = content(fs, e->ino, st.size, &out->failed);
    if (data == NULL)
        return;
    for (len = st.size; len > 0 && data[len - 1] == 0xff; len--)
        ;
    sha256(data, (size_t)len, digest);
    free(data);
    if (len == 0)
        return;
    python_name_hex(name_hex, e->name, e->name_len);
    hex(digest_hex, digest, 32);
    snprintf(line, sizeof(line), "volume %s %llu %s", name_hex, (unsigned long long)len, digest_hex);
    add_line(out, line);
}

static const char *sort_key(const char *line)
{
    int field = strncmp(line, "volume ", 7) == 0 ? 1 : 4;

    while (field-- > 0 && (line = strchr(line, ' ')) != NULL)
        line++;
    return line != NULL ? line : "";
}

static int compare_lines(const void *a, const void *b)
{
    return strcmp(sort_key(*(char *const *)a), sort_key(*(char *const *)b));
}

static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long len;

    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
        return NULL;
    data = malloc(len ? (size_t)len : 1);
    if (data == NULL || fread(data, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)len;
    return data;
}

static int load_expected(const char *path, char *format, size_t format_len, limits_t *limits, lines_t *out)
{
    FILE *f = fopen(path, "r");
    char line[LINE_MAX_LEN];
    size_t len;

    if (f == NULL)
        return -1;
    if (fgets(line, sizeof(line), f) == NULL || strncmp(line, "format ", 7) != 0) {
        fclose(f);
        return -1;
    }
    line[strcspn(line, "\n")] = 0;
    snprintf(format, format_len, "%.31s", line + 7);
    if (fgets(line, sizeof(line), f) == NULL ||
        sscanf(line, "limits %llu %llu %llu", &limits->total, &limits->file, &limits->entries) != 3) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        len = strcspn(line, "\n");
        line[len] = 0;
        if (len > 0)
            add_line(out, line);
    }
    fclose(f);
    return 0;
}

static int format_id(const char *name)
{
    if (strcmp(name, "jffs2") == 0)
        return MTDF_FORMAT_JFFS2;
    if (strcmp(name, "cramfs") == 0)
        return MTDF_FORMAT_CRAMFS;
    return MTDF_FORMAT_UBI;
}

static int damaged(const mtdf_fs_t *fs)
{
    mtdf_report_t r;

    mtdf_report(fs, &r);
    return r.bad_header || r.bad_node_crc || r.bad_data_crc || r.bad_name || r.bad_structure || r.orphans ||
           r.loops || r.duplicates || r.unknown;
}

static void actual_lines(const unsigned char *image, size_t len, int format, const limits_t *limits, lines_t *out)
{
    mtdf_options_t opts;
    mtdf_fs_t *fs;
    mtdf_dirent_t e;
    uint64_t cookie = 0;
    char path[MTDF_PATH_MAX * 2];
    int status;

    memset(&opts, 0, sizeof(opts));
    opts.format = format;
    opts.max_total_bytes = limits->total;
    opts.max_file_bytes = limits->file;
    opts.max_entries = limits->entries;
    status = mtdf_open_mem(image, len, &opts, &fs);
    if (status != MTDF_OK) {
        if (verbose)
            printf("    open: %s\n", mtdf_strerror(status));
        out->failed = 1;
        return;
    }
    out->damaged = damaged(fs);
    if (format == MTDF_FORMAT_UBI) {
        while (mtdf_readdir(fs, MTDF_ROOT, &cookie, &e) == MTDF_OK)
            volume_line(fs, &e, out);
    } else {
        walk(fs, MTDF_ROOT, path, 0, out);
    }
    mtdf_close(fs);
}

static void show(const char *label, const lines_t *l)
{
    size_t i;

    printf("  %s (%zu)%s%s\n", label, l->count, l->failed ? " [refused]" : "", l->damaged ? " [damaged]" : "");
    for (i = 0; i < l->count && i < 40; i++)
        printf("    %s\n", l->text[i]);
}

static int check(const char *image_path)
{
    char expected_path[4096];
    char format[32];
    limits_t limits;
    lines_t want = {{0}, 0, 0, 0};
    lines_t got = {{0}, 0, 0, 0};
    unsigned char *image;
    size_t len = 0;
    size_t i;
    int python_error;
    int same;

    snprintf(expected_path, sizeof(expected_path), "%.*s.txt", (int)(strlen(image_path) - 4), image_path);
    image = slurp(image_path, &len);
    if (image == NULL || load_expected(expected_path, format, sizeof(format), &limits, &want) != 0) {
        printf("FAIL %s: cannot load\n", image_path);
        free(image);
        return 1;
    }
    actual_lines(image, len, format_id(format), &limits, &got);
    free(image);
    python_error = want.count == 1 && strncmp(want.text[0], "error ", 6) == 0;
    qsort(got.text, got.count, sizeof(char *), compare_lines);
    qsort(want.text, want.count, sizeof(char *), compare_lines);
    same = python_error ? got.failed || got.damaged : !got.failed && got.count == want.count;
    for (i = 0; same && !python_error && i < got.count; i++)
        same = strcmp(got.text[i], want.text[i]) == 0;
    if (!same) {
        printf("FAIL %s (%s)\n", image_path, format);
        show("python", &want);
        show("flashfs", &got);
    }
    free_lines(&want);
    free_lines(&got);
    return !same;
}

int main(int argc, char **argv)
{
    char path[4096];
    FILE *list;
    size_t total = 0;
    size_t failed = 0;

    if (argc < 2 || (list = fopen(argv[1], "r")) == NULL) {
        fprintf(stderr, "usage: test_parity <list file> [-v]\n");
        return 2;
    }
    verbose = argc > 2 && strcmp(argv[2], "-v") == 0;
    while (fgets(path, sizeof(path), list) != NULL) {
        path[strcspn(path, "\r\n")] = 0;
        if (path[0] == 0)
            continue;
        total++;
        failed += (size_t)check(path);
    }
    fclose(list);
    printf("parity: %zu images, %zu differ\n", total, failed);
    return failed != 0;
}
