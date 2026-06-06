#include "flashfs_int.h"

#include <stdlib.h>

#define MTDF_J_MAGIC 0x1985u
#define MTDF_J_ACCURATE 0x2000u
#define MTDF_J_DIRENT 0xe001u
#define MTDF_J_INODE 0xe002u
#define MTDF_J_CLEANMARKER 0x2003u
#define MTDF_J_PADDING 0x2004u
#define MTDF_J_SUMMARY 0x2006u
#define MTDF_J_XATTR 0xe008u
#define MTDF_J_XREF 0xe009u
#define MTDF_J_HEADER 12u
#define MTDF_J_DIRENT_SIZE 40u
#define MTDF_J_INODE_SIZE 68u
#define MTDF_J_DIRENT_CRC_SPAN 32u
#define MTDF_J_INODE_CRC_SPAN 60u
#define MTDF_J_ROOT_INO 1u
#define MTDF_J_MAX_DATA (128u * 1024u)
#define MTDF_J_SCAN_CHUNK (64u * 1024u)

#define MTDF_J_COMPR_NONE 0u
#define MTDF_J_COMPR_ZERO 1u
#define MTDF_J_COMPR_RTIME 2u
#define MTDF_J_COMPR_RUBINMIPS 3u
#define MTDF_J_COMPR_COPY 4u
#define MTDF_J_COMPR_DYNRUBIN 5u
#define MTDF_J_COMPR_ZLIB 6u
#define MTDF_J_COMPR_LZO 7u
#define MTDF_J_COMPR_LZMA 8u

typedef struct mtdf_jdirent {
    uint64_t pos;
    uint32_t pino;
    uint32_t ino;
    uint32_t version;
    uint32_t name_off;
    uint32_t entry;
    uint8_t name_len;
    uint8_t type;
    uint8_t state;
    uint8_t unused;
} mtdf_jdirent_t;

typedef struct mtdf_jnode {
    uint64_t pos;
    uint32_t ino;
    uint32_t version;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t isize;
    uint32_t mtime;
    uint32_t offset;
    uint32_t csize;
    uint32_t dsize;
    uint32_t compr;
} mtdf_jnode_t;

typedef struct mtdf_jgroup {
    uint32_t ino;
    uint32_t first;
    uint32_t count;
    uint32_t used;
} mtdf_jgroup_t;

typedef struct mtdf_jffs2 {
    int big;
    uint64_t start;
    uint64_t end;
    mtdf_vec_t dirents;
    mtdf_vec_t dnames;
    mtdf_vec_t nodes;
    mtdf_vec_t groups;
    mtdf_vec_t cleanmarkers;
    uint32_t crossed;
    mtdf_vec_t dirs;
    mtdf_vec_t chain;
    uint32_t perm_group;
    uint32_t perm_max;
    mtdf_vec_t perm;
    mtdf_vec_t hits;
} mtdf_jffs2_t;

static mtdf_jffs2_t *mtdf_j(mtdf_fs_t *fs)
{
    return (mtdf_jffs2_t *)fs->state;
}

static mtdf_jnode_t *mtdf_j_node(mtdf_jffs2_t *j, uint32_t index)
{
    return (mtdf_jnode_t *)j->nodes.data + index;
}

static mtdf_jgroup_t *mtdf_j_group(mtdf_jffs2_t *j, uint32_t index)
{
    return (mtdf_jgroup_t *)j->groups.data + index;
}

static mtdf_jdirent_t *mtdf_j_dirent(mtdf_jffs2_t *j, uint32_t index)
{
    return (mtdf_jdirent_t *)j->dirents.data + index;
}

static const char *mtdf_j_name(const mtdf_jffs2_t *j, const mtdf_jdirent_t *d)
{
    return (const char *)j->dnames.data + d->name_off;
}

static uint32_t mtdf_j_align(uint32_t value)
{
    return (value + 3u) & ~3u;
}

static int mtdf_j_known(uint32_t nodetype)
{
    switch (nodetype | MTDF_J_ACCURATE) {
    case MTDF_J_DIRENT:
    case MTDF_J_INODE:
    case MTDF_J_CLEANMARKER:
    case MTDF_J_PADDING:
    case MTDF_J_SUMMARY:
    case MTDF_J_XATTR:
    case MTDF_J_XREF:
        return 1;
    default:
        return 0;
    }
}

static int mtdf_j_header_ok(const mtdf_fs_t *fs, const uint8_t *h, int big)
{
    uint8_t copy[8];
    uint32_t nodetype = mtdf_get16(h + 2, big) | MTDF_J_ACCURATE;

    memcpy(copy, h, 8);
    copy[big ? 2 : 3] = (uint8_t)(nodetype >> 8);
    copy[big ? 3 : 2] = (uint8_t)nodetype;
    return mtdf_crc(fs, 0, copy, 8) == mtdf_get32(h + 8, big);
}

static int mtdf_j_magic_at(const uint8_t *p, int big)
{
    return big ? p[0] == 0x19 && p[1] == 0x85 : p[0] == 0x85 && p[1] == 0x19;
}

static int mtdf_j_next(mtdf_fs_t *fs, uint64_t pos, int big, uint64_t *out_pos)
{
    const uint8_t *data;
    size_t piece;
    size_t i;
    int status;

    while (pos + 2 <= fs->len) {
        piece = fs->len - pos < MTDF_J_SCAN_CHUNK ? (size_t)(fs->len - pos) : MTDF_J_SCAN_CHUNK;
        status = mtdf_src_get(fs, pos, piece, NULL, &data);
        if (status != MTDF_OK)
            return status;
        for (i = 0; i + 2 <= piece; i += 4) {
            if (mtdf_j_magic_at(data + i, big)) {
                *out_pos = pos + i;
                return MTDF_OK;
            }
        }
        pos += (piece + 3u) & ~(size_t)3u;
    }
    *out_pos = fs->len;
    return MTDF_OK;
}

static int mtdf_j_dirent_parse(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint64_t pos, uint32_t totlen)
{
    const uint8_t *n;
    mtdf_jdirent_t *d;
    uint32_t nsize;
    uint32_t keep;
    int status;

    if (totlen < MTDF_J_DIRENT_SIZE)
        return fs->report.bad_header++, MTDF_OK;
    status = mtdf_src_get(fs, pos, totlen < MTDF_J_DIRENT_SIZE + 255 ? totlen : MTDF_J_DIRENT_SIZE + 255, NULL, &n);
    if (status != MTDF_OK)
        return status;
    nsize = n[28];
    if (MTDF_J_DIRENT_SIZE + nsize > totlen)
        return fs->report.bad_header++, MTDF_OK;
    if (mtdf_crc(fs, 0, n, MTDF_J_DIRENT_CRC_SPAN) != mtdf_get32(n + 32, j->big))
        return fs->report.bad_node_crc++, MTDF_OK;
    if (mtdf_crc(fs, 0, n + 40, nsize) != mtdf_get32(n + 36, j->big))
        return fs->report.bad_data_crc++, MTDF_OK;
    keep = (uint32_t)mtdf_strnlen((const char *)n + 40, nsize);
    if (!mtdf_name_ok((const char *)n + 40, keep))
        return fs->report.bad_name++, MTDF_OK;
    status = mtdf_count(fs, 1);
    if (status == MTDF_OK)
        status = mtdf_vec_grow(fs, &j->dirents, sizeof(mtdf_jdirent_t), 1);
    if (status == MTDF_OK)
        status = mtdf_vec_grow(fs, &j->dnames, 1, keep);
    if (status != MTDF_OK)
        return status;
    d = mtdf_j_dirent(j, (uint32_t)j->dirents.len++);
    memset(d, 0, sizeof(*d));
    d->pos = pos;
    d->pino = mtdf_get32(n + 12, j->big);
    d->version = mtdf_get32(n + 16, j->big);
    d->ino = mtdf_get32(n + 20, j->big);
    d->type = n[29];
    d->name_len = (uint8_t)keep;
    d->name_off = (uint32_t)j->dnames.len;
    d->entry = MTDF_NONE;
    memcpy((char *)j->dnames.data + j->dnames.len, n + 40, keep);
    j->dnames.len += keep;
    return MTDF_OK;
}

static void mtdf_j_inode_fields(const uint8_t *n, int big, mtdf_jnode_t *node)
{
    node->ino = mtdf_get32(n + 12, big);
    node->version = mtdf_get32(n + 16, big);
    node->mode = mtdf_get32(n + 20, big);
    node->uid = mtdf_get16(n + 24, big);
    node->gid = mtdf_get16(n + 26, big);
    node->isize = mtdf_get32(n + 28, big);
    node->mtime = mtdf_get32(n + 36, big);
    node->offset = mtdf_get32(n + 44, big);
    node->csize = mtdf_get32(n + 48, big);
    node->dsize = mtdf_get32(n + 52, big);
    node->compr = n[56];
}

static int mtdf_j_inode_parse(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint64_t pos, uint32_t totlen)
{
    const uint8_t *n;
    mtdf_jnode_t node;
    uint32_t want;
    uint32_t crc;
    int status;

    if (totlen < MTDF_J_INODE_SIZE)
        return fs->report.bad_header++, MTDF_OK;
    status = mtdf_src_get(fs, pos, MTDF_J_INODE_SIZE, NULL, &n);
    if (status != MTDF_OK)
        return status;
    if (mtdf_crc(fs, 0, n, MTDF_J_INODE_CRC_SPAN) != mtdf_get32(n + 64, j->big))
        return fs->report.bad_node_crc++, MTDF_OK;
    mtdf_j_inode_fields(n, j->big, &node);
    node.pos = pos;
    want = mtdf_get32(n + 60, j->big);
    if (node.dsize > MTDF_J_MAX_DATA || node.csize > MTDF_J_MAX_DATA || node.csize > totlen - MTDF_J_INODE_SIZE)
        return fs->report.bad_structure++, MTDF_OK;
    if (node.csize > 0) {
        status = mtdf_src_crc(fs, pos + MTDF_J_INODE_SIZE, node.csize, 0, &crc);
        if (status != MTDF_OK)
            return status;
        if (crc != want)
            return fs->report.bad_data_crc++, MTDF_OK;
    }
    status = mtdf_count(fs, 1);
    if (status == MTDF_OK)
        status = mtdf_vec_grow(fs, &j->nodes, sizeof(mtdf_jnode_t), 1);
    if (status != MTDF_OK)
        return status;
    *mtdf_j_node(j, (uint32_t)j->nodes.len++) = node;
    return MTDF_OK;
}

static int mtdf_j_record(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint64_t pos, uint32_t nodetype, uint32_t totlen)
{
    int status;

    fs->report.nodes++;
    if (nodetype == MTDF_J_DIRENT)
        return mtdf_j_dirent_parse(fs, j, pos, totlen);
    if (nodetype == MTDF_J_INODE)
        return mtdf_j_inode_parse(fs, j, pos, totlen);
    if (nodetype == MTDF_J_CLEANMARKER) {
        status = mtdf_vec_grow(fs, &j->cleanmarkers, sizeof(uint64_t), 1);
        if (status == MTDF_OK)
            ((uint64_t *)j->cleanmarkers.data)[j->cleanmarkers.len++] = pos;
        return status;
    }
    if (!(nodetype & MTDF_J_ACCURATE))
        fs->report.obsolete++;
    else if (!mtdf_j_known(nodetype))
        fs->report.unknown++;
    return MTDF_OK;
}

static int mtdf_j_header(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint64_t pos, uint32_t *out_type, uint32_t *out_len)
{
    const uint8_t *h;
    uint32_t totlen;

    if (mtdf_src_get(fs, pos, MTDF_J_HEADER, NULL, &h) != MTDF_OK || !mtdf_j_magic_at(h, j->big))
        return 0;
    totlen = mtdf_get32(h + 4, j->big);
    if (!mtdf_j_header_ok(fs, h, j->big) || totlen < MTDF_J_HEADER || totlen > fs->len - pos) {
        fs->report.bad_header++;
        return 0;
    }
    *out_type = mtdf_get16(h + 2, j->big);
    *out_len = totlen;
    return 1;
}

#define MTDF_J_MIN_ERASE 4096u
#define MTDF_J_MAX_ERASE (16u << 20)

static void mtdf_j_cross(mtdf_jffs2_t *j, uint64_t pos, uint32_t totlen)
{
    uint64_t size;
    uint32_t i;

    for (size = MTDF_J_MIN_ERASE, i = 0; size <= MTDF_J_MAX_ERASE; size <<= 1, i++) {
        if ((pos - j->start) / size != (pos - j->start + totlen - 1) / size)
            j->crossed |= 1u << i;
    }
}

static int mtdf_j_scan(mtdf_fs_t *fs, mtdf_jffs2_t *j)
{
    uint64_t pos = j->start;
    uint32_t nodetype;
    uint32_t totlen;
    int status;

    while (pos + MTDF_J_HEADER <= fs->len) {
        if (!mtdf_j_header(fs, j, pos, &nodetype, &totlen)) {
            status = mtdf_j_next(fs, pos + 4, j->big, &pos);
            if (status != MTDF_OK)
                return status;
            continue;
        }
        status = mtdf_j_record(fs, j, pos, nodetype, totlen);
        if (status != MTDF_OK)
            return status;
        mtdf_j_cross(j, pos, totlen);
        j->end = pos + totlen;
        pos += mtdf_j_align(totlen);
    }
    return MTDF_OK;
}

static int mtdf_j_valid_at(mtdf_fs_t *fs, uint64_t pos, int big)
{
    const uint8_t *h;
    uint32_t totlen;

    if (pos + MTDF_J_HEADER > fs->len || mtdf_src_get(fs, pos, MTDF_J_HEADER, NULL, &h) != MTDF_OK)
        return 0;
    if (!mtdf_j_magic_at(h, big) || !mtdf_j_known(mtdf_get16(h + 2, big)) || !mtdf_j_header_ok(fs, h, big))
        return 0;
    totlen = mtdf_get32(h + 4, big);
    return totlen >= MTDF_J_HEADER && totlen <= fs->len - pos;
}

static int mtdf_j_find_start(mtdf_fs_t *fs, int forced, uint64_t *out_start, int *out_big)
{
    const uint8_t *w;
    uint64_t pos = 0;
    uint64_t cand[2];
    int big;

    while (!forced && pos + 4 <= fs->len) {
        if (mtdf_src_get(fs, pos, 4, NULL, &w) != MTDF_OK)
            return MTDF_ERR_FORMAT;
        if (mtdf_le32(w) != 0xffffffffu)
            break;
        pos += 4;
    }
    for (big = 0; !forced && big < 2; big++) {
        if (mtdf_j_valid_at(fs, pos, big)) {
            *out_start = pos;
            *out_big = big;
            return MTDF_OK;
        }
    }
    if (!forced)
        return MTDF_ERR_FORMAT;
    for (big = 0; big < 2; big++) {
        cand[big] = 0;
        for (;;) {
            if (mtdf_j_next(fs, cand[big], big, &cand[big]) != MTDF_OK)
                cand[big] = fs->len;
            if (cand[big] >= fs->len || mtdf_j_valid_at(fs, cand[big], big))
                break;
            cand[big] += 4;
        }
    }
    big = cand[1] < cand[0];
    if (cand[big] >= fs->len)
        return MTDF_ERR_FORMAT;
    *out_start = cand[big];
    *out_big = big;
    return MTDF_OK;
}

static int mtdf_j_sort(mtdf_fs_t *fs, void *base, size_t count, size_t size, mtdf_cmp_fn cmp, const void *ctx)
{
    void *tmp;

    if (count < 2)
        return MTDF_OK;
    if (count > SIZE_MAX / size)
        return MTDF_ERR_NOMEM;
    tmp = mtdf_alloc(fs, count * size);
    if (tmp == NULL)
        return MTDF_ERR_NOMEM;
    mtdf_sort(base, tmp, count, size, cmp, ctx);
    mtdf_free(fs, tmp);
    return MTDF_OK;
}

static int mtdf_j_by_ino_version(const void *ctx, const void *pa, const void *pb)
{
    const mtdf_jnode_t *a = pa;
    const mtdf_jnode_t *b = pb;

    (void)ctx;
    if (a->ino != b->ino)
        return a->ino < b->ino ? -1 : 1;
    if (a->version != b->version)
        return a->version < b->version ? -1 : 1;
    return a->pos < b->pos ? -1 : a->pos > b->pos;
}

static int mtdf_j_by_name(const mtdf_jffs2_t *j, const mtdf_jdirent_t *a, const mtdf_jdirent_t *b)
{
    size_t common = a->name_len < b->name_len ? a->name_len : b->name_len;
    int order = memcmp(mtdf_j_name(j, a), mtdf_j_name(j, b), common);

    if (order != 0)
        return order;
    return a->name_len < b->name_len ? -1 : a->name_len > b->name_len;
}

static int mtdf_j_by_parent_name_version(const void *ctx, const void *pa, const void *pb)
{
    const mtdf_jdirent_t *a = pa;
    const mtdf_jdirent_t *b = pb;
    int order;

    if (a->pino != b->pino)
        return a->pino < b->pino ? -1 : 1;
    order = mtdf_j_by_name(ctx, a, b);
    if (order != 0)
        return order;
    if (a->version != b->version)
        return a->version < b->version ? -1 : 1;
    return a->pos < b->pos ? -1 : a->pos > b->pos;
}

static int mtdf_j_by_offset(const void *ctx, const void *pa, const void *pb)
{
    const mtdf_jnode_t *a = mtdf_j_node((mtdf_jffs2_t *)ctx, *(const uint32_t *)pa);
    const mtdf_jnode_t *b = mtdf_j_node((mtdf_jffs2_t *)ctx, *(const uint32_t *)pb);

    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static int mtdf_j_by_index(const void *ctx, const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;

    (void)ctx;
    return a < b ? -1 : a > b;
}

static int mtdf_j_groups(mtdf_fs_t *fs, mtdf_jffs2_t *j)
{
    mtdf_jgroup_t *g;
    uint32_t i;
    int status;

    status = mtdf_j_sort(fs, j->nodes.data, j->nodes.len, sizeof(mtdf_jnode_t), mtdf_j_by_ino_version, NULL);
    for (i = 0; status == MTDF_OK && i < j->nodes.len; i++) {
        if (i > 0 && mtdf_j_node(j, i)->ino == mtdf_j_node(j, i - 1)->ino) {
            mtdf_j_group(j, (uint32_t)j->groups.len - 1)->count++;
            fs->report.superseded += (uint64_t)(mtdf_j_node(j, i - 1)->dsize == 0);
            continue;
        }
        status = mtdf_vec_grow(fs, &j->groups, sizeof(mtdf_jgroup_t), 1);
        if (status != MTDF_OK)
            break;
        g = mtdf_j_group(j, (uint32_t)j->groups.len++);
        g->ino = mtdf_j_node(j, i)->ino;
        g->first = i;
        g->count = 1;
        g->used = 0;
    }
    return status;
}

static uint32_t mtdf_j_find_group(mtdf_jffs2_t *j, uint32_t ino)
{
    uint32_t lo = 0;
    uint32_t hi = (uint32_t)j->groups.len;
    uint32_t mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (mtdf_j_group(j, mid)->ino == ino)
            return mid;
        if (mtdf_j_group(j, mid)->ino < ino)
            lo = mid + 1;
        else
            hi = mid;
    }
    return MTDF_NONE;
}

static const mtdf_jnode_t *mtdf_j_latest(mtdf_jffs2_t *j, uint32_t group)
{
    const mtdf_jgroup_t *g = mtdf_j_group(j, group);

    return mtdf_j_node(j, g->first + g->count - 1);
}

static uint32_t mtdf_j_mode(mtdf_jffs2_t *j, const mtdf_jdirent_t *d)
{
    uint32_t group = mtdf_j_find_group(j, d->ino);

    return group == MTDF_NONE ? (uint32_t)d->type << 12 : mtdf_j_latest(j, group)->mode;
}

static int mtdf_j_same_slot(const mtdf_jffs2_t *j, const mtdf_jdirent_t *a, const mtdf_jdirent_t *b)
{
    return a->pino == b->pino && mtdf_j_by_name(j, a, b) == 0;
}

static int mtdf_j_live_dirents(mtdf_fs_t *fs, mtdf_jffs2_t *j)
{
    size_t kept = 0;
    size_t i;
    int status;

    status = mtdf_j_sort(fs, j->dirents.data, j->dirents.len, sizeof(mtdf_jdirent_t),
                         mtdf_j_by_parent_name_version, j);
    if (status != MTDF_OK)
        return status;
    for (i = 0; i < j->dirents.len; i++) {
        if (i + 1 < j->dirents.len &&
            mtdf_j_same_slot(j, mtdf_j_dirent(j, (uint32_t)i), mtdf_j_dirent(j, (uint32_t)i + 1))) {
            fs->report.superseded++;
            continue;
        }
        if (mtdf_j_dirent(j, (uint32_t)i)->ino != 0)
            *mtdf_j_dirent(j, (uint32_t)kept++) = *mtdf_j_dirent(j, (uint32_t)i);
    }
    j->dirents.len = kept;
    return MTDF_OK;
}

static int mtdf_j_by_dir_ino(const void *ctx, const void *pa, const void *pb)
{
    uint32_t a = mtdf_j_dirent((mtdf_jffs2_t *)ctx, *(const uint32_t *)pa)->ino;
    uint32_t b = mtdf_j_dirent((mtdf_jffs2_t *)ctx, *(const uint32_t *)pb)->ino;

    return a < b ? -1 : a > b;
}

static int mtdf_j_canonical_dirs(mtdf_fs_t *fs, mtdf_jffs2_t *j)
{
    uint32_t *dirs;
    size_t kept = 0;
    size_t i;
    int status;

    for (i = 0; i < j->dirents.len; i++) {
        if ((mtdf_j_mode(j, mtdf_j_dirent(j, (uint32_t)i)) & MTDF_S_IFMT) != MTDF_S_IFDIR)
            continue;
        status = mtdf_vec_grow(fs, &j->dirs, sizeof(uint32_t), 1);
        if (status != MTDF_OK)
            return status;
        ((uint32_t *)j->dirs.data)[j->dirs.len++] = (uint32_t)i;
    }
    status = mtdf_j_sort(fs, j->dirs.data, j->dirs.len, sizeof(uint32_t), mtdf_j_by_dir_ino, j);
    dirs = j->dirs.data;
    for (i = 0; status == MTDF_OK && i < j->dirs.len; i++) {
        if (i + 1 < j->dirs.len && mtdf_j_dirent(j, dirs[i])->ino == mtdf_j_dirent(j, dirs[i + 1])->ino) {
            fs->report.loops++;
            continue;
        }
        dirs[kept++] = dirs[i];
    }
    j->dirs.len = kept;
    return status;
}

static uint32_t mtdf_j_dir_of(mtdf_jffs2_t *j, uint32_t ino)
{
    const uint32_t *dirs = j->dirs.data;
    uint32_t lo = 0;
    uint32_t hi = (uint32_t)j->dirs.len;
    uint32_t mid;
    uint32_t found;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        found = mtdf_j_dirent(j, dirs[mid])->ino;
        if (found == ino)
            return dirs[mid];
        if (found < ino)
            lo = mid + 1;
        else
            hi = mid;
    }
    return MTDF_NONE;
}

#define MTDF_J_NEW 0
#define MTDF_J_WALKING 1
#define MTDF_J_PLACED 2
#define MTDF_J_DROPPED 3

static int mtdf_j_resolve(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint32_t start)
{
    uint32_t cur = start;
    uint32_t parent;
    uint8_t outcome = MTDF_J_DROPPED;
    uint64_t *reason = &fs->report.orphans;
    int status;

    j->chain.len = 0;
    for (;;) {
        mtdf_jdirent_t *d = mtdf_j_dirent(j, cur);
        if (d->state == MTDF_J_PLACED || d->state == MTDF_J_DROPPED) {
            outcome = d->state;
            break;
        }
        if (d->state == MTDF_J_WALKING) {
            reason = &fs->report.loops;
            break;
        }
        status = mtdf_vec_grow(fs, &j->chain, sizeof(uint32_t), 1);
        if (status != MTDF_OK)
            return status;
        ((uint32_t *)j->chain.data)[j->chain.len++] = cur;
        d->state = MTDF_J_WALKING;
        if (d->pino == MTDF_J_ROOT_INO) {
            outcome = MTDF_J_PLACED;
            break;
        }
        parent = mtdf_j_dir_of(j, d->pino);
        if (parent == MTDF_NONE)
            break;
        cur = parent;
    }
    while (j->chain.len > 0) {
        mtdf_j_dirent(j, ((uint32_t *)j->chain.data)[--j->chain.len])->state = outcome;
        if (outcome == MTDF_J_DROPPED)
            (*reason)++;
    }
    return MTDF_OK;
}

static int mtdf_j_compressed(mtdf_fs_t *fs, const mtdf_jnode_t *node, const uint8_t **out)
{
    return mtdf_src_get(fs, node->pos + MTDF_J_INODE_SIZE, node->csize, fs->unit + MTDF_UNIT_MAX, out);
}

static int mtdf_j_zlib(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    if (in_len >= 2 && (in[0] & 0x0f) == 8 && (((unsigned)in[0] << 8) + in[1]) % 31 == 0)
        return mtdf_inflate(fs, in + 2, in_len - 2, out, out_len, 1);
    return mtdf_inflate(fs, in, in_len, out, out_len, 0);
}

static int mtdf_j_decode(mtdf_fs_t *fs, const mtdf_jnode_t *node, const uint8_t *in, uint8_t *out)
{
    switch (node->compr) {
    case MTDF_J_COMPR_NONE:
        if (node->csize < node->dsize)
            return mtdf_fail(fs, MTDF_ERR_CORRUPT, "jffs2 inode %u stores %u of %u bytes", node->ino,
                             node->csize, node->dsize);
        memcpy(out, in, node->dsize);
        return MTDF_OK;
    case MTDF_J_COMPR_ZERO:
        memset(out, 0, node->dsize);
        return MTDF_OK;
    case MTDF_J_COMPR_RTIME:
        return mtdf_unrtime(fs, in, node->csize, out, node->dsize);
    case MTDF_J_COMPR_ZLIB:
        return mtdf_j_zlib(fs, in, node->csize, out, node->dsize);
    case MTDF_J_COMPR_LZO:
        return mtdf_unlzo(fs, in, node->csize, out, node->dsize);
    case MTDF_J_COMPR_LZMA:
        return mtdf_unlzma_jffs2(fs, in, node->csize, out, node->dsize);
    case MTDF_J_COMPR_RUBINMIPS:
    case MTDF_J_COMPR_DYNRUBIN:
    case MTDF_J_COMPR_COPY:
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "jffs2 compression %u (rubin/copy) is not implemented",
                         node->compr);
    default:
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "jffs2 inode %u has unknown compression %u", node->ino,
                         node->compr);
    }
}

static int mtdf_j_load(mtdf_fs_t *fs, const mtdf_jnode_t *node, const uint8_t **out)
{
    const uint8_t *in = NULL;
    int status;

    if (fs->unit_valid && fs->unit_key == node->pos + 1) {
        *out = fs->unit;
        return MTDF_OK;
    }
    status = mtdf_unit_buffer(fs);
    if (status == MTDF_OK)
        status = mtdf_charge(fs, node->dsize);
    if (status == MTDF_OK && node->csize > 0)
        status = mtdf_j_compressed(fs, node, &in);
    fs->unit_valid = 0;
    if (status == MTDF_OK)
        status = mtdf_j_decode(fs, node, in, fs->unit);
    if (status != MTDF_OK)
        return status;
    fs->unit_valid = 1;
    fs->unit_key = node->pos + 1;
    fs->unit_len = node->dsize;
    *out = fs->unit;
    return MTDF_OK;
}

static int mtdf_j_order(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint32_t group)
{
    const mtdf_jgroup_t *g = mtdf_j_group(j, group);
    uint32_t *perm;
    uint32_t i;
    int status;

    if (j->perm_group == group + 1)
        return MTDF_OK;
    j->perm_group = 0;
    j->perm.len = 0;
    status = mtdf_vec_grow(fs, &j->perm, sizeof(uint32_t), g->count);
    if (status != MTDF_OK)
        return status;
    perm = j->perm.data;
    j->perm_max = 0;
    for (i = 0; i < g->count; i++) {
        perm[i] = g->first + i;
        if (mtdf_j_node(j, g->first + i)->dsize > j->perm_max)
            j->perm_max = mtdf_j_node(j, g->first + i)->dsize;
    }
    j->perm.len = g->count;
    status = mtdf_j_sort(fs, perm, g->count, sizeof(uint32_t), mtdf_j_by_offset, j);
    if (status == MTDF_OK)
        j->perm_group = group + 1;
    return status;
}

static int mtdf_j_hits(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint64_t off, uint64_t end, uint32_t isize)
{
    const uint32_t *perm = j->perm.data;
    const mtdf_jnode_t *node;
    uint64_t lo = off > j->perm_max ? off - j->perm_max : 0;
    size_t first = 0;
    size_t hi = j->perm.len;
    size_t mid;
    int status;

    while (first < hi) {
        mid = first + (hi - first) / 2;
        if (mtdf_j_node(j, perm[mid])->offset < lo)
            first = mid + 1;
        else
            hi = mid;
    }
    j->hits.len = 0;
    for (; first < j->perm.len && mtdf_j_node(j, perm[first])->offset < end; first++) {
        node = mtdf_j_node(j, perm[first]);
        if ((uint64_t)node->offset + node->dsize <= off || node->offset >= isize || node->dsize == 0)
            continue;
        status = mtdf_vec_grow(fs, &j->hits, sizeof(uint32_t), 1);
        if (status != MTDF_OK)
            return status;
        ((uint32_t *)j->hits.data)[j->hits.len++] = perm[first];
    }
    return mtdf_j_sort(fs, j->hits.data, j->hits.len, sizeof(uint32_t), mtdf_j_by_index, NULL);
}

static int mtdf_j_read(mtdf_fs_t *fs, const mtdf_entry_t *entry, uint64_t off, uint8_t *buf, size_t len)
{
    mtdf_jffs2_t *j = mtdf_j(fs);
    const mtdf_jnode_t *node;
    const uint8_t *data;
    uint64_t end = off + len;
    uint64_t from;
    uint64_t to;
    size_t i;
    int status;

    memset(buf, 0, len);
    if (entry->ref == 0)
        return MTDF_OK;
    status = mtdf_j_order(fs, j, (uint32_t)(entry->ref - 1));
    if (status == MTDF_OK)
        status = mtdf_j_hits(fs, j, off, end, (uint32_t)entry->size);
    for (i = 0; status == MTDF_OK && i < j->hits.len; i++) {
        node = mtdf_j_node(j, ((uint32_t *)j->hits.data)[i]);
        status = mtdf_j_load(fs, node, &data);
        if (status != MTDF_OK)
            break;
        from = node->offset > off ? node->offset : off;
        to = (uint64_t)node->offset + node->dsize < end ? (uint64_t)node->offset + node->dsize : end;
        memcpy(buf + (from - off), data + (from - node->offset), (size_t)(to - from));
    }
    return status;
}

static void mtdf_j_device(mtdf_fs_t *fs, mtdf_jffs2_t *j, const mtdf_jnode_t *node, mtdf_entry_t *entry)
{
    const uint8_t *data;
    uint32_t dev;

    if ((node->dsize != 2 && node->dsize != 4) || mtdf_j_load(fs, node, &data) != MTDF_OK) {
        fs->report.bad_structure++;
        return;
    }
    if (node->dsize == 2) {
        dev = mtdf_get16(data, j->big);
        entry->dev_major = dev >> 8;
        entry->dev_minor = dev & 0xffu;
        return;
    }
    dev = mtdf_get32(data, j->big);
    entry->dev_major = (dev & 0xfff00u) >> 8;
    entry->dev_minor = (dev & 0xffu) | ((dev >> 12) & 0xfff00u);
}

static void mtdf_j_attributes(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint32_t ino, uint32_t mode, mtdf_entry_t *entry)
{
    uint32_t group = mtdf_j_find_group(j, ino);
    const mtdf_jnode_t *node;
    uint32_t type;

    entry->mode = mode;
    if (group == MTDF_NONE)
        return;
    node = mtdf_j_latest(j, group);
    mtdf_j_group(j, group)->used++;
    entry->mode = node->mode;
    entry->uid = node->uid;
    entry->gid = node->gid;
    entry->mtime = node->mtime;
    entry->size = node->isize;
    entry->ref = (uint64_t)group + 1;
    type = node->mode & MTDF_S_IFMT;
    if (type == MTDF_S_IFCHR || type == MTDF_S_IFBLK) {
        entry->size = 0;
        mtdf_j_device(fs, j, node, entry);
    }
}

static int mtdf_j_place(mtdf_fs_t *fs, mtdf_jffs2_t *j)
{
    mtdf_jdirent_t *d;
    uint32_t parent;
    uint32_t i;
    int status;

    for (i = 0; i < j->dirents.len; i++) {
        d = mtdf_j_dirent(j, i);
        if (d->state != MTDF_J_PLACED)
            continue;
        status = mtdf_tree_add(fs, mtdf_j_name(j, d), d->name_len, &d->entry);
        if (status != MTDF_OK)
            return status;
        mtdf_j_attributes(fs, j, d->ino, (uint32_t)d->type << 12, mtdf_entry(fs, d->entry));
    }
    for (i = 0; i < j->dirents.len; i++) {
        d = mtdf_j_dirent(j, i);
        if (d->state != MTDF_J_PLACED || d->pino == MTDF_J_ROOT_INO)
            continue;
        parent = mtdf_j_dir_of(j, d->pino);
        mtdf_entry(fs, d->entry)->parent = mtdf_j_dirent(j, parent)->entry;
    }
    return MTDF_OK;
}

static void mtdf_j_links_and_orphans(mtdf_fs_t *fs, mtdf_jffs2_t *j)
{
    mtdf_entry_t *entry;
    uint32_t i;

    for (i = 1; i < mtdf_entry_count(fs); i++) {
        entry = mtdf_entry(fs, i);
        if (entry->ref != 0)
            entry->nlink = mtdf_j_group(j, (uint32_t)(entry->ref - 1))->used;
    }
    for (i = 0; i < j->groups.len; i++) {
        if (mtdf_j_group(j, i)->used == 0 && mtdf_j_group(j, i)->ino != MTDF_J_ROOT_INO)
            fs->report.orphans++;
    }
}

static int mtdf_j_build(mtdf_fs_t *fs, mtdf_jffs2_t *j)
{
    uint32_t i;
    int status;

    status = mtdf_j_groups(fs, j);
    if (status == MTDF_OK)
        status = mtdf_j_live_dirents(fs, j);
    if (status == MTDF_OK)
        status = mtdf_j_canonical_dirs(fs, j);
    for (i = 0; status == MTDF_OK && i < j->dirents.len; i++)
        status = mtdf_j_resolve(fs, j, i);
    if (status == MTDF_OK)
        status = mtdf_j_place(fs, j);
    if (status != MTDF_OK)
        return status;
    mtdf_j_attributes(fs, j, MTDF_J_ROOT_INO, MTDF_S_IFDIR | 0755, mtdf_entry(fs, MTDF_ROOT));
    mtdf_entry(fs, MTDF_ROOT)->mode = MTDF_S_IFDIR | (mtdf_entry(fs, MTDF_ROOT)->mode & 07777);
    mtdf_entry(fs, MTDF_ROOT)->ref = 0;
    mtdf_j_links_and_orphans(fs, j);
    return MTDF_OK;
}

static uint64_t mtdf_j_gcd(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static uint32_t mtdf_j_erase_size(mtdf_jffs2_t *j, uint32_t crossed)
{
    const uint64_t *cm = j->cleanmarkers.data;
    uint64_t g = 0;
    uint32_t size;
    size_t i;

    for (i = 1; i < j->cleanmarkers.len; i++)
        g = mtdf_j_gcd(g, cm[i] - cm[i - 1]);
    if (g >= MTDF_J_MIN_ERASE && g <= MTDF_J_MAX_ERASE && (g & (g - 1)) == 0)
        return (uint32_t)g;
    for (size = MTDF_J_MIN_ERASE, i = 0; size <= MTDF_J_MAX_ERASE; size <<= 1, i++) {
        if (!(crossed & (1u << i)))
            return size;
    }
    return 0;
}

static int mtdf_j_geometry(mtdf_fs_t *fs, mtdf_jffs2_t *j, uint32_t crossed)
{
    uint64_t block;
    int erased;
    int status;

    fs->info.format = MTDF_FORMAT_JFFS2;
    fs->info.big_endian = (uint32_t)j->big;
    fs->info.image_size = (j->end + 3u) & ~(uint64_t)3u;
    if (fs->info.image_size > fs->len)
        fs->info.image_size = fs->len;
    fs->info.block_size = mtdf_j_erase_size(j, crossed);
    if (fs->info.block_size == 0)
        return MTDF_OK;
    for (block = j->start; fs->len - block >= fs->info.block_size; block += fs->info.block_size) {
        status = mtdf_all_ff(fs, block, fs->info.block_size, &erased);
        if (status != MTDF_OK)
            return status;
        fs->report.erased_blocks += (uint64_t)erased;
    }
    return MTDF_OK;
}

static void mtdf_j_destroy(mtdf_fs_t *fs)
{
    mtdf_jffs2_t *j = mtdf_j(fs);

    mtdf_vec_free(fs, &j->dirents);
    mtdf_vec_free(fs, &j->dnames);
    mtdf_vec_free(fs, &j->nodes);
    mtdf_vec_free(fs, &j->groups);
    mtdf_vec_free(fs, &j->cleanmarkers);
    mtdf_vec_free(fs, &j->dirs);
    mtdf_vec_free(fs, &j->chain);
    mtdf_vec_free(fs, &j->perm);
    mtdf_vec_free(fs, &j->hits);
    mtdf_free(fs, j);
    fs->state = NULL;
}

static const mtdf_driver_t mtdf_jffs2_driver = {mtdf_j_read, mtdf_j_destroy};

int mtdf_jffs2_open(mtdf_fs_t *fs, int forced)
{
    mtdf_jffs2_t *j;
    uint64_t start;
    int big;
    int status;

    if (mtdf_j_find_start(fs, forced, &start, &big) != MTDF_OK)
        return MTDF_ERR_FORMAT;
    j = mtdf_alloc(fs, sizeof(*j));
    if (j == NULL)
        return MTDF_ERR_NOMEM;
    memset(j, 0, sizeof(*j));
    j->big = big;
    j->start = start;
    fs->state = j;
    fs->driver = &mtdf_jffs2_driver;
    status = mtdf_j_scan(fs, j);
    if (status == MTDF_OK)
        status = mtdf_j_build(fs, j);
    if (status == MTDF_OK)
        status = mtdf_j_geometry(fs, j, j->crossed);
    mtdf_vec_free(fs, &j->dirents);
    mtdf_vec_free(fs, &j->dnames);
    mtdf_vec_free(fs, &j->dirs);
    mtdf_vec_free(fs, &j->chain);
    mtdf_vec_free(fs, &j->cleanmarkers);
    return status;
}
