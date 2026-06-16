#include "flashfs_int.h"

#include <stdio.h>

#define MTDF_U_EC_MAGIC 0x55424923u
#define MTDF_U_VID_MAGIC 0x55424921u
#define MTDF_U_HEADER 64u
#define MTDF_U_LAYOUT 0x7fffefffu
#define MTDF_U_INTERNAL 0x7fffef00u
#define MTDF_U_MAX_VOLUMES 128u
#define MTDF_U_RECORD 172u
#define MTDF_U_RECORD_CRC 168u
#define MTDF_U_NAME_MAX 127u
#define MTDF_U_DYNAMIC 1u
#define MTDF_U_STATIC 2u
#define MTDF_U_MIN_PEB (16u * 1024u)
#define MTDF_U_MAX_PEB (2u * 1024u * 1024u)
#define MTDF_U_CRC_SEED 0xffffffffu

typedef struct mtdf_uleb {
    uint64_t sqnum;
    uint32_t peb;
    uint32_t vol;
    uint32_t lnum;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t data_pad;
    uint32_t data_crc;
    uint32_t used_ebs;
    uint8_t type;
    uint8_t copy;
    uint8_t unused[2];
} mtdf_uleb_t;

typedef struct mtdf_uvol {
    uint64_t size;
    uint32_t id;
    uint32_t first;
    uint32_t count;
    uint32_t usable;
    uint32_t reserved;
    uint32_t data_pad;
    uint8_t type;
    uint8_t in_table;
    uint8_t name_len;
    char name[MTDF_U_NAME_MAX + 1];
} mtdf_uvol_t;

typedef struct mtdf_ubi {
    uint32_t peb_size;
    uint32_t pebs;
    uint32_t vid_offset;
    uint32_t data_offset;
    mtdf_vec_t lebs;
    mtdf_vec_t vols;
} mtdf_ubi_t;

static mtdf_ubi_t *mtdf_u(mtdf_fs_t *fs)
{
    return (mtdf_ubi_t *)fs->state;
}

static mtdf_uleb_t *mtdf_u_leb(mtdf_ubi_t *u, size_t index)
{
    return (mtdf_uleb_t *)u->lebs.data + index;
}

static mtdf_uvol_t *mtdf_u_vol(mtdf_ubi_t *u, size_t index)
{
    return (mtdf_uvol_t *)u->vols.data + index;
}

static int mtdf_u_header_crc_ok(const mtdf_fs_t *fs, const uint8_t *h)
{
    return mtdf_crc(fs, MTDF_U_CRC_SEED, h, MTDF_U_HEADER - 4) == mtdf_be32(h + MTDF_U_HEADER - 4);
}

static int mtdf_u_ec(mtdf_fs_t *fs, uint64_t pos, uint32_t peb_size, uint32_t *out_vid, uint32_t *out_data)
{
    const uint8_t *h;
    uint32_t vid;
    uint32_t data;

    if (pos + MTDF_U_HEADER > fs->len || mtdf_src_get(fs, pos, MTDF_U_HEADER, NULL, &h) != MTDF_OK)
        return 0;
    if (mtdf_be32(h) != MTDF_U_EC_MAGIC || h[4] != 1 || !mtdf_u_header_crc_ok(fs, h))
        return 0;
    vid = mtdf_be32(h + 16);
    data = mtdf_be32(h + 20);
    if (data < 2 * MTDF_U_HEADER || data >= peb_size || vid < MTDF_U_HEADER || vid > data - MTDF_U_HEADER)
        return 0;
    *out_vid = vid;
    *out_data = data;
    return 1;
}

static int mtdf_u_magic_at(mtdf_fs_t *fs, uint64_t pos, uint32_t magic)
{
    const uint8_t *h;

    return pos + 4 <= fs->len && mtdf_src_get(fs, pos, 4, NULL, &h) == MTDF_OK && mtdf_be32(h) == magic;
}

static uint32_t mtdf_u_peb_size(mtdf_fs_t *fs)
{
    uint32_t size;

    for (size = MTDF_U_MIN_PEB; size <= MTDF_U_MAX_PEB; size <<= 1) {
        if (mtdf_u_magic_at(fs, size, MTDF_U_EC_MAGIC))
            return size;
    }
    return 0;
}

static uint32_t mtdf_u_peb_count(mtdf_fs_t *fs, uint32_t peb_size)
{
    uint64_t count = 0;

    while ((count + 1) * peb_size <= fs->len) {
        if (!mtdf_u_magic_at(fs, count * peb_size, MTDF_U_EC_MAGIC) &&
            !mtdf_u_magic_at(fs, count * peb_size, 0xffffffffu))
            break;
        count++;
    }
    return count > MTDF_NONE - 1 ? MTDF_NONE - 1 : (uint32_t)count;
}

static void mtdf_u_vid_fields(const uint8_t *h, mtdf_uleb_t *leb)
{
    leb->type = h[5];
    leb->copy = h[6];
    leb->vol = mtdf_be32(h + 8);
    leb->lnum = mtdf_be32(h + 12);
    leb->data_size = mtdf_be32(h + 20);
    leb->used_ebs = mtdf_be32(h + 24);
    leb->data_pad = mtdf_be32(h + 28);
    leb->data_crc = mtdf_be32(h + 32);
    leb->sqnum = mtdf_be64(h + 40);
}

static int mtdf_u_vid(mtdf_fs_t *fs, mtdf_ubi_t *u, uint64_t pos, mtdf_uleb_t *leb)
{
    const uint8_t *h;
    uint32_t room = u->peb_size - leb->data_offset;
    int erased;

    if (mtdf_src_get(fs, pos, MTDF_U_HEADER, NULL, &h) != MTDF_OK)
        return fs->report.bad_structure++, 0;
    if (mtdf_be32(h) != MTDF_U_VID_MAGIC) {
        if (mtdf_all_ff(fs, pos, MTDF_U_HEADER, &erased) != MTDF_OK || !erased)
            fs->report.bad_header++;
        return 0;
    }
    if (!mtdf_u_header_crc_ok(fs, h))
        return fs->report.bad_header++, 0;
    mtdf_u_vid_fields(h, leb);
    fs->report.nodes++;
    if (leb->vol >= MTDF_U_INTERNAL && leb->vol != MTDF_U_LAYOUT)
        return fs->report.unknown++, 0;
    if ((leb->vol >= MTDF_U_MAX_VOLUMES && leb->vol != MTDF_U_LAYOUT) ||
        (leb->type != MTDF_U_DYNAMIC && leb->type != MTDF_U_STATIC) || leb->data_pad >= room ||
        (leb->type == MTDF_U_STATIC && (leb->data_size > room - leb->data_pad || leb->used_ebs == 0 ||
                                        leb->lnum >= leb->used_ebs)))
        return fs->report.bad_structure++, 0;
    return 1;
}

static int mtdf_u_scan_peb(mtdf_fs_t *fs, mtdf_ubi_t *u, uint32_t peb)
{
    uint64_t pos = (uint64_t)peb * u->peb_size;
    mtdf_uleb_t leb;
    uint32_t vid = u->vid_offset;
    int erased;
    int status;

    memset(&leb, 0, sizeof(leb));
    leb.data_offset = u->data_offset;
    if (!mtdf_u_magic_at(fs, pos, MTDF_U_EC_MAGIC)) {
        status = mtdf_all_ff(fs, pos, u->peb_size, &erased);
        fs->report.erased_blocks += (uint64_t)(status == MTDF_OK && erased);
        return status;
    }
    if (!mtdf_u_ec(fs, pos, u->peb_size, &vid, &leb.data_offset))
        fs->report.bad_header++;
    if (!mtdf_u_vid(fs, u, pos + vid, &leb))
        return MTDF_OK;
    leb.peb = peb;
    status = mtdf_vec_grow(fs, &u->lebs, sizeof(mtdf_uleb_t), 1);
    if (status == MTDF_OK)
        *mtdf_u_leb(u, u->lebs.len++) = leb;
    return status;
}

static int mtdf_u_scan(mtdf_fs_t *fs, mtdf_ubi_t *u)
{
    uint32_t peb;
    int status;

    for (peb = 0; peb < u->pebs; peb++) {
        status = mtdf_count(fs, 1);
        if (status == MTDF_OK)
            status = mtdf_u_scan_peb(fs, u, peb);
        if (status != MTDF_OK)
            return status;
    }
    return MTDF_OK;
}

static int mtdf_u_leb_order(const mtdf_uleb_t *a, const mtdf_uleb_t *b)
{
    if (a->vol != b->vol)
        return a->vol < b->vol ? -1 : 1;
    if (a->lnum != b->lnum)
        return a->lnum < b->lnum ? -1 : 1;
    if (a->sqnum != b->sqnum)
        return a->sqnum > b->sqnum ? -1 : 1;
    return a->peb < b->peb ? -1 : a->peb > b->peb;
}

static int mtdf_u_sort(mtdf_fs_t *fs, mtdf_ubi_t *u)
{
    mtdf_uleb_t *a = u->lebs.data;
    mtdf_uleb_t *tmp;
    size_t n = u->lebs.len;
    size_t width;
    size_t lo;
    size_t i;
    size_t m;
    size_t k;

    if (n < 2)
        return MTDF_OK;
    tmp = mtdf_alloc(fs, n * sizeof(*tmp));
    if (tmp == NULL)
        return MTDF_ERR_NOMEM;
    for (width = 1; width < n; width *= 2) {
        for (lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            for (i = lo, m = mid, k = lo; k < hi; k++)
                tmp[k] = (i < mid && (m >= hi || mtdf_u_leb_order(a + i, a + m) <= 0)) ? a[i++] : a[m++];
        }
        memcpy(a, tmp, n * sizeof(*tmp));
    }
    mtdf_free(fs, tmp);
    return MTDF_OK;
}

static int mtdf_u_data_ok(mtdf_fs_t *fs, mtdf_ubi_t *u, const mtdf_uleb_t *leb, int *out_ok)
{
    uint32_t crc;
    int status;

    *out_ok = 1;
    if (leb->type != MTDF_U_STATIC && !leb->copy)
        return MTDF_OK;
    if (leb->data_size > u->peb_size - leb->data_offset) {
        *out_ok = 0;
        return MTDF_OK;
    }
    status = mtdf_src_crc(fs, (uint64_t)leb->peb * u->peb_size + leb->data_offset, leb->data_size,
                          MTDF_U_CRC_SEED, &crc);
    if (status == MTDF_OK)
        *out_ok = crc == leb->data_crc;
    return status;
}

static int mtdf_u_winners(mtdf_fs_t *fs, mtdf_ubi_t *u)
{
    size_t kept = 0;
    size_t i = 0;
    size_t j;
    int found;
    int ok;
    int status;

    while (i < u->lebs.len) {
        for (j = i, found = 0; j < u->lebs.len && mtdf_u_leb(u, j)->vol == mtdf_u_leb(u, i)->vol &&
                               mtdf_u_leb(u, j)->lnum == mtdf_u_leb(u, i)->lnum;
             j++) {
            if (found) {
                fs->report.superseded++;
                continue;
            }
            status = mtdf_u_data_ok(fs, u, mtdf_u_leb(u, j), &ok);
            if (status != MTDF_OK)
                return status;
            if (!ok) {
                fs->report.bad_data_crc++;
                continue;
            }
            *mtdf_u_leb(u, kept++) = *mtdf_u_leb(u, j);
            found = 1;
        }
        i = j;
    }
    u->lebs.len = kept;
    return MTDF_OK;
}

static uint32_t mtdf_u_find(mtdf_ubi_t *u, uint32_t first, uint32_t count, uint32_t lnum)
{
    uint32_t lo = first;
    uint32_t hi = first + count;
    uint32_t mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (mtdf_u_leb(u, mid)->lnum == lnum)
            return mid;
        if (mtdf_u_leb(u, mid)->lnum < lnum)
            lo = mid + 1;
        else
            hi = mid;
    }
    return MTDF_NONE;
}

static int mtdf_u_add_volume(mtdf_fs_t *fs, mtdf_ubi_t *u, uint32_t id, mtdf_uvol_t **out)
{
    mtdf_uvol_t *v;
    int status;

    status = mtdf_vec_grow(fs, &u->vols, sizeof(mtdf_uvol_t), 1);
    if (status != MTDF_OK)
        return status;
    v = mtdf_u_vol(u, u->vols.len++);
    memset(v, 0, sizeof(*v));
    v->id = id;
    v->first = MTDF_NONE;
    *out = v;
    return MTDF_OK;
}

static mtdf_uvol_t *mtdf_u_volume(mtdf_ubi_t *u, uint32_t id)
{
    size_t i;

    for (i = 0; i < u->vols.len; i++) {
        if (mtdf_u_vol(u, i)->id == id)
            return mtdf_u_vol(u, i);
    }
    return NULL;
}

static int mtdf_u_record(mtdf_fs_t *fs, mtdf_ubi_t *u, const mtdf_uvol_t *layout, uint32_t index,
                         const uint8_t **out)
{
    uint32_t copy;
    uint32_t at;
    const mtdf_uleb_t *leb;

    for (copy = 0; copy < 2; copy++) {
        at = mtdf_u_find(u, layout->first, layout->count, copy);
        if (at == MTDF_NONE)
            continue;
        leb = mtdf_u_leb(u, at);
        if ((uint64_t)(index + 1) * MTDF_U_RECORD > u->peb_size - leb->data_offset)
            continue;
        if (mtdf_src_get(fs, (uint64_t)leb->peb * u->peb_size + leb->data_offset + index * MTDF_U_RECORD,
                         MTDF_U_RECORD, NULL, out) != MTDF_OK)
            continue;
        if (mtdf_crc(fs, MTDF_U_CRC_SEED, *out, MTDF_U_RECORD_CRC) == mtdf_be32(*out + MTDF_U_RECORD_CRC))
            return 1;
    }
    return 0;
}

static int mtdf_u_name_taken(mtdf_ubi_t *u, const uint8_t *name, size_t len)
{
    size_t i;

    for (i = 0; i < u->vols.len; i++) {
        if (mtdf_u_vol(u, i)->name_len == len && memcmp(mtdf_u_vol(u, i)->name, name, len) == 0)
            return 1;
    }
    return 0;
}

static int mtdf_u_table_entry(mtdf_fs_t *fs, mtdf_ubi_t *u, uint32_t id, const uint8_t *r)
{
    uint32_t name_len = mtdf_be16(r + 14);
    mtdf_uvol_t *v;
    int status;

    if (mtdf_be32(r) == 0)
        return MTDF_OK;
    status = mtdf_u_add_volume(fs, u, id, &v);
    if (status != MTDF_OK)
        return status;
    v->in_table = 1;
    v->reserved = mtdf_be32(r);
    v->data_pad = mtdf_be32(r + 8);
    v->type = r[12];
    if (name_len == 0 || name_len > MTDF_U_NAME_MAX || !mtdf_name_ok((const char *)r + 16, name_len) ||
        memchr(r + 16, 0, name_len) != NULL) {
        fs->report.bad_name++;
        return MTDF_OK;
    }
    if (mtdf_u_name_taken(u, r + 16, name_len)) {
        fs->report.duplicates++;
        return MTDF_OK;
    }
    memcpy(v->name, r + 16, name_len);
    v->name_len = (uint8_t)name_len;
    return MTDF_OK;
}

static int mtdf_u_table(mtdf_fs_t *fs, mtdf_ubi_t *u, const mtdf_uvol_t *layout)
{
    uint32_t slots = (u->peb_size - u->data_offset) / MTDF_U_RECORD;
    const uint8_t *r;
    uint32_t id;
    int erased;
    int status;

    for (id = 0; id < slots && id < MTDF_U_MAX_VOLUMES; id++) {
        if (!mtdf_u_record(fs, u, layout, id, &r)) {
            if (mtdf_u_find(u, layout->first, layout->count, 0) != MTDF_NONE &&
                (mtdf_all_ff(fs, (uint64_t)mtdf_u_leb(u, layout->first)->peb * u->peb_size +
                                     mtdf_u_leb(u, layout->first)->data_offset + id * MTDF_U_RECORD,
                             MTDF_U_RECORD, &erased) != MTDF_OK ||
                 !erased))
                fs->report.bad_structure++;
            continue;
        }
        status = mtdf_u_table_entry(fs, u, id, r);
        if (status != MTDF_OK)
            return status;
    }
    return MTDF_OK;
}

static int mtdf_u_attach(mtdf_fs_t *fs, mtdf_ubi_t *u, int have_table)
{
    mtdf_uvol_t *v;
    size_t i = 0;
    size_t j;
    int status;

    while (i < u->lebs.len) {
        for (j = i; j < u->lebs.len && mtdf_u_leb(u, j)->vol == mtdf_u_leb(u, i)->vol; j++)
            ;
        v = mtdf_u_volume(u, mtdf_u_leb(u, i)->vol);
        if (v == NULL) {
            status = mtdf_u_add_volume(fs, u, mtdf_u_leb(u, i)->vol, &v);
            if (status != MTDF_OK)
                return status;
            v->type = mtdf_u_leb(u, i)->type;
            v->data_pad = mtdf_u_leb(u, i)->data_pad;
            fs->report.orphans += (uint64_t)(have_table && v->id != MTDF_U_LAYOUT);
        }
        v->first = (uint32_t)i;
        v->count = (uint32_t)(j - i);
        i = j;
    }
    return MTDF_OK;
}

static void mtdf_u_size(mtdf_fs_t *fs, mtdf_ubi_t *u, mtdf_uvol_t *v)
{
    const mtdf_uleb_t *last;
    uint32_t used;
    uint32_t at;

    v->usable = u->peb_size - u->data_offset - (v->data_pad < u->peb_size - u->data_offset ? v->data_pad : 0);
    if (v->count == 0) {
        v->size = (uint64_t)v->reserved * v->usable;
        return;
    }
    last = mtdf_u_leb(u, v->first + v->count - 1);
    if (v->type == MTDF_U_STATIC && last->type == MTDF_U_STATIC) {
        used = last->used_ebs;
        at = mtdf_u_find(u, v->first, v->count, used - 1);
        v->size = (uint64_t)(used - 1) * v->usable +
                  (at == MTDF_NONE ? v->usable : mtdf_u_leb(u, at)->data_size);
        return;
    }
    if (v->in_table && last->lnum >= v->reserved)
        fs->report.bad_structure++;
    v->size = (uint64_t)(v->in_table && v->reserved > last->lnum ? v->reserved : last->lnum + 1u) * v->usable;
}

static int mtdf_u_files(mtdf_fs_t *fs, mtdf_ubi_t *u)
{
    char fallback[24];
    mtdf_uvol_t *v;
    mtdf_entry_t *e;
    uint32_t index;
    size_t i;
    int status;

    for (i = 0; i < u->vols.len; i++) {
        v = mtdf_u_vol(u, i);
        if (v->id == MTDF_U_LAYOUT)
            continue;
        mtdf_u_size(fs, u, v);
        if (v->name_len == 0)
            snprintf(fallback, sizeof(fallback), "volume@%u", v->id);
        status = v->name_len ? mtdf_tree_add(fs, v->name, v->name_len, &index)
                             : mtdf_tree_add(fs, fallback, strlen(fallback), &index);
        if (status != MTDF_OK)
            return status;
        e = mtdf_entry(fs, index);
        e->mode = MTDF_S_IFREG | 0444;
        e->size = v->size;
        e->ref = (uint64_t)i + 1;
    }
    return MTDF_OK;
}

static int mtdf_u_read(mtdf_fs_t *fs, const mtdf_entry_t *entry, uint64_t off, uint8_t *buf, size_t len)
{
    mtdf_ubi_t *u = mtdf_u(fs);
    const mtdf_uvol_t *v = mtdf_u_vol(u, (size_t)(entry->ref - 1));
    const mtdf_uleb_t *leb;
    const uint8_t *p;
    uint64_t lnum;
    size_t inner;
    size_t piece;
    uint32_t at;
    int status;

    while (len > 0) {
        lnum = off / v->usable;
        inner = (size_t)(off % v->usable);
        piece = v->usable - inner < len ? v->usable - inner : len;
        piece = piece < MTDF_SMALL ? piece : MTDF_SMALL;
        at = lnum > UINT32_MAX ? MTDF_NONE : mtdf_u_find(u, v->first, v->count, (uint32_t)lnum);
        if (at == MTDF_NONE) {
            memset(buf, 0xff, piece);
        } else {
            leb = mtdf_u_leb(u, at);
            status = mtdf_src_get(fs, (uint64_t)leb->peb * u->peb_size + leb->data_offset + inner, piece, NULL, &p);
            if (status != MTDF_OK)
                return status;
            memcpy(buf, p, piece);
        }
        buf += piece;
        off += piece;
        len -= piece;
    }
    return MTDF_OK;
}

static void mtdf_u_destroy(mtdf_fs_t *fs)
{
    mtdf_ubi_t *u = mtdf_u(fs);

    mtdf_vec_free(fs, &u->lebs);
    mtdf_vec_free(fs, &u->vols);
    mtdf_free(fs, u);
    fs->state = NULL;
}

static const mtdf_driver_t mtdf_ubi_driver = {mtdf_u_read, mtdf_u_destroy};

static int mtdf_u_build(mtdf_fs_t *fs, mtdf_ubi_t *u)
{
    mtdf_uvol_t *layout;
    int status;

    status = mtdf_u_scan(fs, u);
    if (status == MTDF_OK)
        status = mtdf_u_sort(fs, u);
    if (status == MTDF_OK)
        status = mtdf_u_winners(fs, u);
    if (status == MTDF_OK)
        status = mtdf_u_attach(fs, u, 0);
    if (status != MTDF_OK)
        return status;
    layout = mtdf_u_volume(u, MTDF_U_LAYOUT);
    if (layout != NULL) {
        mtdf_uvol_t saved = *layout;
        u->vols.len = 0;
        status = mtdf_u_table(fs, u, &saved);
        if (status == MTDF_OK)
            status = mtdf_u_attach(fs, u, 1);
    }
    if (status == MTDF_OK)
        status = mtdf_u_files(fs, u);
    return status;
}

int mtdf_ubi_open(mtdf_fs_t *fs, int forced)
{
    mtdf_ubi_t *u;
    uint32_t peb_size;
    uint32_t vid;
    uint32_t data;

    (void)forced;
    peb_size = mtdf_u_peb_size(fs);
    if (peb_size == 0 || !mtdf_u_ec(fs, 0, peb_size, &vid, &data))
        return MTDF_ERR_FORMAT;
    u = mtdf_alloc(fs, sizeof(*u));
    if (u == NULL)
        return MTDF_ERR_NOMEM;
    memset(u, 0, sizeof(*u));
    u->peb_size = peb_size;
    u->pebs = mtdf_u_peb_count(fs, peb_size);
    u->vid_offset = vid;
    u->data_offset = data;
    fs->state = u;
    fs->driver = &mtdf_ubi_driver;
    fs->info.format = MTDF_FORMAT_UBI;
    fs->info.big_endian = 1;
    fs->info.image_size = (uint64_t)u->pebs * peb_size;
    fs->info.block_size = peb_size;
    fs->info.unit_size = peb_size - data;
    return mtdf_u_build(fs, u);
}
