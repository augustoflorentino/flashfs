#include "flashfs_int.h"

#include <stdlib.h>

uint32_t mtdf_type_of(uint32_t mode)
{
    switch (mode & MTDF_S_IFMT) {
    case MTDF_S_IFREG:
        return MTDF_TYPE_FILE;
    case MTDF_S_IFDIR:
        return MTDF_TYPE_DIR;
    case MTDF_S_IFLNK:
        return MTDF_TYPE_LINK;
    case MTDF_S_IFCHR:
        return MTDF_TYPE_CHR;
    case MTDF_S_IFBLK:
        return MTDF_TYPE_BLK;
    case MTDF_S_IFIFO:
        return MTDF_TYPE_FIFO;
    case MTDF_S_IFSOCK:
        return MTDF_TYPE_SOCK;
    default:
        return MTDF_TYPE_OTHER;
    }
}

int mtdf_name_ok(const char *name, size_t name_len)
{
    if (name_len == 0 || memchr(name, '/', name_len) != NULL || memchr(name, '\0', name_len) != NULL)
        return 0;
    return !(name_len == 1 && name[0] == '.') && !(name_len == 2 && name[0] == '.' && name[1] == '.');
}

static const char *mtdf_name(const mtdf_fs_t *fs, const mtdf_entry_t *entry)
{
    return (const char *)fs->names.data + entry->name_off;
}

int mtdf_tree_add(mtdf_fs_t *fs, const char *name, size_t name_len, uint32_t *out_index)
{
    mtdf_entry_t *entry;
    int status;

    if (fs->entries.len >= MTDF_NONE - 1 || fs->names.len + name_len + 1 >= MTDF_NONE)
        return mtdf_fail(fs, MTDF_ERR_BUDGET, "tree larger than the index can address");
    status = mtdf_count(fs, 1);
    if (status == MTDF_OK)
        status = mtdf_vec_grow(fs, &fs->entries, sizeof(mtdf_entry_t), 1);
    if (status == MTDF_OK)
        status = mtdf_vec_grow(fs, &fs->names, 1, name_len + 1);
    if (status != MTDF_OK)
        return status;
    entry = mtdf_entry(fs, (uint32_t)fs->entries.len);
    memset(entry, 0, sizeof(*entry));
    entry->name_off = (uint32_t)fs->names.len;
    entry->name_len = (uint32_t)name_len;
    entry->nlink = 1;
    entry->parent = MTDF_ROOT;
    if (name_len > 0)
        memcpy((char *)fs->names.data + fs->names.len, name, name_len);
    ((char *)fs->names.data)[fs->names.len + name_len] = '\0';
    fs->names.len += name_len + 1;
    *out_index = (uint32_t)fs->entries.len++;
    return MTDF_OK;
}

int mtdf_tree_init(mtdf_fs_t *fs)
{
    uint32_t root;
    int status;

    status = mtdf_tree_add(fs, "", 0, &root);
    if (status != MTDF_OK)
        return status;
    mtdf_entry(fs, root)->mode = MTDF_S_IFDIR | 0755;
    return MTDF_OK;
}

typedef struct mtdf_sort_key {
    uint32_t parent;
    uint32_t index;
} mtdf_sort_key_t;

static int mtdf_by_parent_name(const void *ctx, const void *pa, const void *pb)
{
    const mtdf_fs_t *fs = ctx;
    const mtdf_sort_key_t *a = pa;
    const mtdf_sort_key_t *b = pb;
    const mtdf_entry_t *ea = mtdf_entry((mtdf_fs_t *)fs, a->index);
    const mtdf_entry_t *eb = mtdf_entry((mtdf_fs_t *)fs, b->index);
    size_t common = ea->name_len < eb->name_len ? ea->name_len : eb->name_len;
    int order;

    if (a->parent != b->parent)
        return a->parent < b->parent ? -1 : 1;
    order = memcmp(mtdf_name(fs, ea), mtdf_name(fs, eb), common);
    if (order != 0)
        return order;
    if (ea->name_len != eb->name_len)
        return ea->name_len < eb->name_len ? -1 : 1;
    return a->index < b->index ? -1 : a->index > b->index;
}

void mtdf_sort(void *items, void *tmp, size_t count, size_t size, mtdf_cmp_fn cmp, const void *ctx)
{
    uint8_t *a = items;
    uint8_t *t = tmp;
    size_t width;
    size_t lo;
    size_t mid;
    size_t hi;
    size_t i;
    size_t j;
    size_t k;

    for (width = 1; width < count; width *= 2) {
        for (lo = 0; lo < count; lo += 2 * width) {
            mid = lo + width < count ? lo + width : count;
            hi = lo + 2 * width < count ? lo + 2 * width : count;
            for (i = lo, j = mid, k = lo; k < hi; k++) {
                if (i < mid && (j >= hi || cmp(ctx, a + i * size, a + j * size) <= 0))
                    memcpy(t + k * size, a + i++ * size, size);
                else
                    memcpy(t + k * size, a + j++ * size, size);
            }
        }
        memcpy(a, t, count * size);
    }
}

static int mtdf_same_name(const mtdf_fs_t *fs, uint32_t a, uint32_t b)
{
    const mtdf_entry_t *ea = mtdf_entry((mtdf_fs_t *)fs, a);
    const mtdf_entry_t *eb = mtdf_entry((mtdf_fs_t *)fs, b);

    return ea->parent == eb->parent && ea->name_len == eb->name_len &&
           memcmp(mtdf_name(fs, ea), mtdf_name(fs, eb), ea->name_len) == 0;
}

static void mtdf_link_children(mtdf_fs_t *fs, const mtdf_sort_key_t *keys, size_t count)
{
    mtdf_entry_t *parent;
    uint32_t placed = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        if (i + 1 < count && mtdf_same_name(fs, keys[i].index, keys[i + 1].index)) {
            fs->report.duplicates++;
            continue;
        }
        parent = mtdf_entry(fs, keys[i].parent);
        if (parent->child_count == 0)
            parent->first_child = placed;
        parent->child_count++;
        fs->children[placed++] = keys[i].index;
    }
}

static int mtdf_walk_depth(mtdf_fs_t *fs, uint32_t *stack, uint32_t *depth)
{
    uint32_t top = 0;
    uint32_t dir;
    uint32_t child;
    uint32_t i;
    mtdf_entry_t *entry;

    stack[top++] = MTDF_ROOT;
    depth[MTDF_ROOT] = 0;
    mtdf_entry(fs, MTDF_ROOT)->linked = 1;
    while (top > 0) {
        dir = stack[--top];
        for (i = 0; i < mtdf_entry(fs, dir)->child_count; i++) {
            child = fs->children[mtdf_entry(fs, dir)->first_child + i];
            entry = mtdf_entry(fs, child);
            if (entry->linked)
                continue;
            entry->linked = 1;
            depth[child] = depth[dir] + 1;
            if (depth[child] > fs->max_depth)
                return mtdf_fail(fs, MTDF_ERR_BUDGET, "directory depth above %u", fs->max_depth);
            if ((entry->mode & MTDF_S_IFMT) == MTDF_S_IFDIR) {
                mtdf_entry(fs, dir)->nlink++;
                stack[top++] = child;
            }
        }
    }
    return MTDF_OK;
}

int mtdf_tree_finish(mtdf_fs_t *fs)
{
    size_t count = fs->entries.len - 1;
    mtdf_sort_key_t *keys;
    uint32_t *scratch;
    size_t i;
    int status = MTDF_ERR_NOMEM;

    if (fs->entries.len > SIZE_MAX / (2 * sizeof(mtdf_sort_key_t)))
        return MTDF_ERR_NOMEM;
    keys = mtdf_alloc(fs, 2 * fs->entries.len * sizeof(*keys));
    fs->children = mtdf_alloc(fs, fs->entries.len * sizeof(uint32_t));
    if (keys != NULL && fs->children != NULL) {
        for (i = 0; i < count; i++) {
            keys[i].index = (uint32_t)(i + 1);
            keys[i].parent = mtdf_entry(fs, (uint32_t)(i + 1))->parent;
        }
        mtdf_sort(keys, keys + fs->entries.len, count, sizeof(*keys), mtdf_by_parent_name, fs);
        for (i = 0; i < fs->entries.len; i++)
            mtdf_entry(fs, (uint32_t)i)->nlink = (mtdf_entry(fs, (uint32_t)i)->mode & MTDF_S_IFMT) == MTDF_S_IFDIR
                                                     ? 2
                                                     : mtdf_entry(fs, (uint32_t)i)->nlink;
        mtdf_link_children(fs, keys, count);
        scratch = (uint32_t *)keys;
        status = mtdf_walk_depth(fs, scratch, scratch + fs->entries.len);
    }
    mtdf_free(fs, keys);
    return status;
}

static int mtdf_child(mtdf_fs_t *fs, uint32_t dir, const char *name, size_t name_len, uint32_t *out)
{
    const mtdf_entry_t *parent = mtdf_entry(fs, dir);
    const mtdf_entry_t *entry;
    uint32_t lo = 0;
    uint32_t hi = parent->child_count;
    uint32_t mid;
    size_t common;
    int order;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        entry = mtdf_entry(fs, fs->children[parent->first_child + mid]);
        common = entry->name_len < name_len ? entry->name_len : name_len;
        order = memcmp(mtdf_name(fs, entry), name, common);
        if (order == 0 && entry->name_len != name_len)
            order = entry->name_len < name_len ? -1 : 1;
        if (order == 0) {
            *out = fs->children[parent->first_child + mid];
            return MTDF_OK;
        }
        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return MTDF_ERR_NOENT;
}

static int mtdf_splice_link(mtdf_fs_t *fs, uint32_t link, const char *rest, size_t *out_len)
{
    size_t target_len;
    size_t rest_len = strlen(rest);
    int status;

    status = mtdf_readlink(fs, link, fs->path[1], sizeof(fs->path[1]), &target_len);
    if (status == MTDF_ERR_RANGE)
        status = MTDF_ERR_NAMETOOLONG;
    if (status != MTDF_OK)
        return status;
    if (target_len + 1 + rest_len > MTDF_PATH_MAX)
        return MTDF_ERR_NAMETOOLONG;
    fs->path[1][target_len] = '/';
    memcpy(fs->path[1] + target_len + 1, rest, rest_len + 1);
    memcpy(fs->path[0], fs->path[1], target_len + rest_len + 2);
    *out_len = target_len + 1 + rest_len;
    return MTDF_OK;
}

static int mtdf_walk_path(mtdf_fs_t *fs, uint32_t flags, mtdf_ino_t *out_ino)
{
    const char *p = fs->path[0];
    const char *end;
    uint32_t cur = MTDF_ROOT;
    uint32_t next;
    unsigned links = 0;
    size_t len;
    int status;

    for (;;) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        end = p;
        while (*end != '\0' && *end != '/')
            end++;
        len = (size_t)(end - p);
        if ((mtdf_entry(fs, cur)->mode & MTDF_S_IFMT) != MTDF_S_IFDIR)
            return MTDF_ERR_NOTDIR;
        if (len == 1 && p[0] == '.') {
            p = end;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            cur = mtdf_entry(fs, cur)->parent;
            p = end;
            continue;
        }
        status = mtdf_child(fs, cur, p, len, &next);
        if (status != MTDF_OK)
            return status;
        while (*end == '/')
            end++;
        if ((mtdf_entry(fs, next)->mode & MTDF_S_IFMT) != MTDF_S_IFLNK || (*end == '\0' && !(flags & MTDF_LOOKUP_FOLLOW))) {
            cur = next;
            p = end;
            continue;
        }
        if (++links > MTDF_SYMLINK_MAX)
            return MTDF_ERR_LOOP;
        status = mtdf_splice_link(fs, next, end, &len);
        if (status != MTDF_OK)
            return status;
        if (fs->path[0][0] == '/')
            cur = MTDF_ROOT;
        p = fs->path[0];
    }
    *out_ino = cur;
    return MTDF_OK;
}

int mtdf_lookup(mtdf_fs_t *fs, const char *path, uint32_t flags, mtdf_ino_t *out_ino)
{
    size_t len;

    if (fs == NULL || path == NULL || out_ino == NULL || (flags & ~MTDF_LOOKUP_FOLLOW) != 0)
        return MTDF_ERR_ARG;
    fs->detail[0] = '\0';
    for (len = 0; len <= MTDF_PATH_MAX && path[len] != '\0'; len++)
        ;
    if (len > MTDF_PATH_MAX)
        return MTDF_ERR_NAMETOOLONG;
    memcpy(fs->path[0], path, len + 1);
    return mtdf_walk_path(fs, flags, out_ino);
}

int mtdf_readdir(mtdf_fs_t *fs, mtdf_ino_t dir, uint64_t *cookie, mtdf_dirent_t *out_entry)
{
    const mtdf_entry_t *parent;
    const mtdf_entry_t *entry;
    uint32_t index;

    if (fs == NULL || cookie == NULL || out_entry == NULL || dir >= mtdf_entry_count(fs))
        return MTDF_ERR_ARG;
    parent = mtdf_entry(fs, dir);
    if ((parent->mode & MTDF_S_IFMT) != MTDF_S_IFDIR)
        return MTDF_ERR_NOTDIR;
    if (*cookie >= parent->child_count)
        return MTDF_END;
    index = fs->children[parent->first_child + (uint32_t)*cookie];
    entry = mtdf_entry(fs, index);
    memset(out_entry, 0, sizeof(*out_entry));
    out_entry->name = mtdf_name(fs, entry);
    out_entry->name_len = entry->name_len;
    out_entry->ino = index;
    out_entry->type = mtdf_type_of(entry->mode);
    (*cookie)++;
    return MTDF_OK;
}

int mtdf_xattr_add(mtdf_fs_t *fs, const mtdf_xrec_t *rec, const char *prefix, const char *name, size_t name_len)
{
    size_t prefix_len = strlen(prefix);
    size_t len = prefix_len + name_len;
    char *at;
    mtdf_xrec_t *x;
    int status;

    if (len == 0 || len > MTDF_XATTR_NAME_MAX || memchr(name, 0, name_len) != NULL)
        return fs->report.bad_name++, MTDF_OK;
    if (fs->xnames.len + len + 1 >= MTDF_NONE)
        return mtdf_fail(fs, MTDF_ERR_BUDGET, "xattr names larger than the index can address");
    status = mtdf_count(fs, 1);
    if (status == MTDF_OK)
        status = mtdf_vec_grow(fs, &fs->xattrs, sizeof(mtdf_xrec_t), 1);
    if (status == MTDF_OK)
        status = mtdf_vec_grow(fs, &fs->xnames, 1, len + 1);
    if (status != MTDF_OK)
        return status;
    x = mtdf_xattr(fs, fs->xattrs.len++);
    *x = *rec;
    x->name_off = (uint32_t)fs->xnames.len;
    x->name_len = (uint32_t)len;
    at = (char *)fs->xnames.data + fs->xnames.len;
    memcpy(at, prefix, prefix_len);
    if (name_len > 0)
        memcpy(at + prefix_len, name, name_len);
    at[len] = '\0';
    fs->xnames.len += len + 1;
    return MTDF_OK;
}

static int mtdf_xattr_name_order(const mtdf_fs_t *fs, const mtdf_xrec_t *a, const mtdf_xrec_t *b)
{
    const char *names = fs->xnames.data;
    uint32_t common = a->name_len < b->name_len ? a->name_len : b->name_len;
    int order = memcmp(names + a->name_off, names + b->name_off, common);

    if (order != 0)
        return order;
    return a->name_len < b->name_len ? -1 : a->name_len > b->name_len;
}

static int mtdf_by_owner_name_seq(const void *ctx, const void *pa, const void *pb)
{
    const mtdf_xrec_t *a = pa;
    const mtdf_xrec_t *b = pb;
    int order;

    if (a->owner != b->owner)
        return a->owner < b->owner ? -1 : 1;
    order = mtdf_xattr_name_order(ctx, a, b);
    if (order != 0)
        return order;
    return (a->seq > b->seq) - (a->seq < b->seq);
}

int mtdf_xattr_finish(mtdf_fs_t *fs)
{
    size_t count = fs->xattrs.len;
    mtdf_xrec_t *tmp;
    size_t kept = 0;
    size_t i;

    if (count == 0)
        return MTDF_OK;
    if (count > SIZE_MAX / sizeof(mtdf_xrec_t))
        return MTDF_ERR_NOMEM;
    tmp = mtdf_alloc(fs, count * sizeof(mtdf_xrec_t));
    if (tmp == NULL)
        return MTDF_ERR_NOMEM;
    mtdf_sort(fs->xattrs.data, tmp, count, sizeof(mtdf_xrec_t), mtdf_by_owner_name_seq, fs);
    mtdf_free(fs, tmp);
    for (i = 0; i < count; i++) {
        if (i + 1 < count && mtdf_xattr(fs, i)->owner == mtdf_xattr(fs, i + 1)->owner &&
            mtdf_xattr_name_order(fs, mtdf_xattr(fs, i), mtdf_xattr(fs, i + 1)) == 0)
            fs->report.superseded++;
        else if (!mtdf_xattr(fs, i)->deleted)
            *mtdf_xattr(fs, kept++) = *mtdf_xattr(fs, i);
    }
    fs->xattrs.len = kept;
    return MTDF_OK;
}

size_t mtdf_xattr_first(const mtdf_fs_t *fs, uint64_t owner)
{
    size_t lo = 0;
    size_t hi = fs->xattrs.len;
    size_t mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (mtdf_xattr(fs, mid)->owner < owner)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
