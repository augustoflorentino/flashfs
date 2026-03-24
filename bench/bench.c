#define _POSIX_C_SOURCE 199309L

#include "../tests/build.h"

#include <time.h>

#define RUNS 7

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    return (x > y) - (x < y);
}

static double median(double *v)
{
    qsort(v, RUNS, sizeof(double), compare_double);
    return v[RUNS / 2];
}

static int list_all(mtdf_fs_t *fs, mtdf_ino_t dir, uint64_t *count)
{
    mtdf_dirent_t e;
    mtdf_stat_t st;
    uint64_t cookie = 0;
    int status;

    while ((status = mtdf_readdir(fs, dir, &cookie, &e)) == MTDF_OK) {
        if ((status = mtdf_stat(fs, e.ino, &st)) != MTDF_OK)
            return status;
        ++*count;
        if (st.type == MTDF_TYPE_DIR && (status = list_all(fs, e.ino, count)) != MTDF_OK)
            return status;
    }
    return status == MTDF_END ? MTDF_OK : status;
}

typedef struct run {
    double open;
    double list;
    double read;
    uint64_t entries;
    uint64_t bytes;
    size_t heap_open;
    size_t heap_list;
    size_t heap_read;
} run_t;

static int one_run(const buf_t *image, const mtdf_options_t *limits, run_t *r)
{
    static unsigned char scratch[1 << 20];
    mtdf_options_t opts = *limits;
    counter_t c;
    mtdf_fs_t *fs;
    double t;
    int status;

    counter_options(&opts, &c, -1);
    if (opts.max_total_bytes == 0)
        opts.max_total_bytes = UINT64_MAX;
    memset(r, 0, sizeof(*r));
    t = now();
    status = mtdf_open_mem(image->data, image->len, &opts, &fs);
    r->open = now() - t;
    if (status != MTDF_OK)
        return status;
    r->heap_open = c.peak;
    t = now();
    status = list_all(fs, MTDF_ROOT, &r->entries);
    r->list = now() - t;
    r->heap_list = c.peak;
    t = now();
    if (status == MTDF_OK)
        status = walk_all(fs, MTDF_ROOT, scratch, sizeof(scratch), UINT64_MAX, &r->bytes);
    r->read = now() - t;
    r->heap_read = c.peak;
    mtdf_close(fs);
    return status;
}

static int bench_image(const char *path)
{
    double open[RUNS];
    double list[RUNS];
    double read[RUNS];
    run_t r;
    buf_t image = {0};
    char dir[4096];
    const char *slash = strrchr(path, '/');
    mtdf_options_t none;
    int i;
    int status = MTDF_OK;

    snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    if (buf_load(&image, dir, slash ? slash + 1 : path) != 0)
        return 1;
    memset(&none, 0, sizeof(none));
    for (i = 0; i < RUNS && status == MTDF_OK; i++) {
        status = one_run(&image, &none, &r);
        open[i] = r.open;
        list[i] = r.list;
        read[i] = r.read;
    }
    if (status != MTDF_OK) {
        printf("%s: %s\n", path, mtdf_strerror(status));
        buf_free(&image);
        return 1;
    }
    printf("%s\n  image %zu bytes, %llu entries, %llu content bytes\n", path, image.len,
           (unsigned long long)r.entries, (unsigned long long)r.bytes);
    printf("  open %.2f ms, list %.2f ms, open+list %.2f ms (median of %d)\n", median(open) * 1e3,
           median(list) * 1e3, (median(open) + median(list)) * 1e3, RUNS);
    printf("  read %.2f ms, %.0f MB/s\n", median(read) * 1e3, (double)r.bytes / median(read) / 1e6);
    printf("  heap peak: open %zu KiB (%.2f%% of image), list %zu KiB (%.2f%%), read %zu KiB\n", r.heap_open >> 10,
           100.0 * (double)r.heap_open / (double)image.len, r.heap_list >> 10,
           100.0 * (double)r.heap_list / (double)image.len, r.heap_read >> 10);
    buf_free(&image);
    return 0;
}

static int corpus_limits(const char *image_path, mtdf_options_t *opts)
{
    char path[4096];
    char line[256];
    unsigned long long total = 0;
    unsigned long long file = 0;
    unsigned long long entries = 0;
    FILE *f;
    int found = 0;

    snprintf(path, sizeof(path), "%.*s.txt", (int)(strlen(image_path) - 4), image_path);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    while (!found && fgets(line, sizeof(line), f) != NULL)
        found = sscanf(line, "limits %llu %llu %llu", &total, &file, &entries) == 3;
    fclose(f);
    memset(opts, 0, sizeof(*opts));
    opts->max_total_bytes = total;
    opts->max_file_bytes = file;
    opts->max_entries = entries;
    return found ? 0 : -1;
}

static int bench_list(const char *list_path)
{
    char line[4096];
    char dir[4096];
    buf_t image = {0};
    mtdf_options_t limits;
    run_t r;
    FILE *list = fopen(list_path, "r");
    double total = 0;
    long images = 0;
    long failed = 0;
    char *slash;

    if (list == NULL)
        return 1;
    while (fgets(line, sizeof(line), list) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        slash = strrchr(line, '/');
        if (slash == NULL)
            continue;
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - line), line);
        if (buf_load(&image, dir, slash + 1) != 0 || corpus_limits(line, &limits) != 0)
            continue;
        failed += one_run(&image, &limits, &r) != MTDF_OK;
        total += r.open + r.list + r.read;
        images++;
    }
    fclose(list);
    buf_free(&image);
    printf("%ld images (%ld refused), open+list+read %.1f ms in total\n", images, failed, total * 1e3);
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    int status = 0;

    if (argc == 3 && strcmp(argv[1], "-l") == 0)
        return bench_list(argv[2]);
    if (argc < 2) {
        fprintf(stderr, "usage: bench <image>... | bench -l <list>\n");
        return 2;
    }
    for (i = 1; i < argc; i++)
        status |= bench_image(argv[i]);
    return status;
}
