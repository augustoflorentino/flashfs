#include "flashfs_int.h"

#define MTDF_F_MAGIC 0x06101831u
#define MTDF_F_CH 24u
#define MTDF_F_BLOCK 4096u
#define MTDF_F_SB_LEN 4096u
#define MTDF_F_MST_LEN 512u
#define MTDF_F_INO_LEN 160u
#define MTDF_F_DENT_LEN 56u
#define MTDF_F_DATA_LEN 48u
#define MTDF_F_TRUN_LEN 56u
#define MTDF_F_IDX_LEN 28u
#define MTDF_F_PAD_LEN 28u
#define MTDF_F_REF_LEN 64u
#define MTDF_F_CS_LEN 32u
#define MTDF_F_BRANCH 20u
#define MTDF_F_MAX_INO_DATA 4096u
#define MTDF_F_MAX_LEVEL 64u
#define MTDF_F_MIN_LEB (15u * 1024u)
#define MTDF_F_MAX_LEB (16u * 1024u * 1024u)
#define MTDF_F_MAX_FORMAT 5u
#define MTDF_F_LOG_LNUM 3u
#define MTDF_F_ROOT_INUM 1u
#define MTDF_F_PADDING_BYTE 0xceu
#define MTDF_F_LEAF 0xfffffffeu

#define MTDF_F_T_INO 0
#define MTDF_F_T_DATA 1
#define MTDF_F_T_DENT 2
#define MTDF_F_T_XENT 3
#define MTDF_F_T_TRUN 4
#define MTDF_F_T_PAD 5
#define MTDF_F_T_SB 6
#define MTDF_F_T_MST 7
#define MTDF_F_T_REF 8
#define MTDF_F_T_IDX 9
#define MTDF_F_T_CS 10

#define MTDF_F_K_INO 0u
#define MTDF_F_K_DATA 1u
#define MTDF_F_K_DENT 2u
#define MTDF_F_K_XENT 3u

#define MTDF_F_FLG_AUTHENTICATION 0x20u
#define MTDF_F_INO_XATTR 0x20u
#define MTDF_F_INO_CRYPT 0x40u

#define MTDF_F_COMPR_NONE 0
#define MTDF_F_COMPR_LZO 1
#define MTDF_F_COMPR_ZLIB 2
#define MTDF_F_COMPR_ZSTD 3

#define MTDF_F_INODE 0u
#define MTDF_F_TRUNC 1u
#define MTDF_F_DATA 2u
#define MTDF_F_DENTRY 3u
#define MTDF_F_XENTRY 4u

#define MTDF_F_NEW 0u
#define MTDF_F_WALKING 1u
#define MTDF_F_PLACED 2u
#define MTDF_F_DROPPED 3u

typedef struct mtdf_frec {
    uint64_t sqnum;
    uint64_t aux;
    uint32_t inum;
    uint32_t kind;
    uint32_t lnum;
    uint32_t offs;
    uint32_t len;
    uint32_t a;
    uint32_t b;
    uint32_t c;
} mtdf_frec_t;

typedef struct mtdf_fino {
    uint32_t inum;
    uint32_t rec;
    uint32_t data_first;
    uint32_t data_count;
    uint32_t dir_dent;
    uint32_t used;
} mtdf_fino_t;

typedef struct mtdf_fdent {
    uint32_t rec;
    uint32_t entry;
    uint32_t state;
} mtdf_fdent_t;

typedef struct mtdf_fbranch {
    uint32_t lnum;
    uint32_t offs;
    uint32_t len;
    uint32_t level;
    uint8_t key[8];
} mtdf_fbranch_t;

typedef struct mtdf_ubifs {
    uint32_t leb_size;
    uint32_t leb_cnt;
    uint32_t log_lebs;
    uint32_t main_first;
    uint32_t fanout;
    uint64_t cmt_no;
    uint32_t log_lnum;
    uint32_t root_lnum;
    uint32_t root_offs;
    uint32_t root_len;
    mtdf_vec_t recs;
    mtdf_vec_t names;
    mtdf_vec_t inodes;
    mtdf_vec_t data;
    mtdf_vec_t dents;
    mtdf_vec_t xents;
    mtdf_vec_t stack;
    mtdf_vec_t buds;
    mtdf_vec_t chain;
} mtdf_ubifs_t;

static mtdf_ubifs_t *mtdf_f(mtdf_fs_t *fs)
{
    return fs->state;
}

static mtdf_frec_t *mtdf_f_rec(const mtdf_ubifs_t *u, uint32_t index)
{
    return (mtdf_frec_t *)u->recs.data + index;
}

static mtdf_fino_t *mtdf_f_ino(const mtdf_ubifs_t *u, uint32_t index)
{
    return (mtdf_fino_t *)u->inodes.data + index;
}

static mtdf_fdent_t *mtdf_f_dent(const mtdf_ubifs_t *u, uint32_t index)
{
    return (mtdf_fdent_t *)u->dents.data + index;
}

static uint32_t mtdf_f_data(const mtdf_ubifs_t *u, uint32_t index)
{
    return ((const uint32_t *)u->data.data)[index];
}

static uint64_t mtdf_f_pos(const mtdf_ubifs_t *u, uint32_t lnum, uint32_t offs)
{
    return (uint64_t)lnum * u->leb_size + offs;
}

static int64_t mtdf_f_time(uint64_t seconds)
{
    return seconds <= (uint64_t)INT64_MAX ? (int64_t)seconds : -(int64_t)(~seconds) - 1;
}

static int mtdf_f_node(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t lnum, uint32_t offs, int count,
                       const uint8_t **out, uint32_t *out_len)
{
    const uint8_t *p;
    uint32_t len;
    int status;

    if (lnum >= u->leb_cnt || offs > u->leb_size || u->leb_size - offs < MTDF_F_CH)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs node at %u:%u outside the volume", lnum, offs);
    status = mtdf_src_get(fs, mtdf_f_pos(u, lnum, offs), MTDF_F_CH, NULL, &p);
    if (status != MTDF_OK)
        return status;
    len = mtdf_le32(p + 16);
    if (mtdf_le32(p) != MTDF_F_MAGIC || len < MTDF_F_CH || len > u->leb_size - offs || len > MTDF_SMALL) {
        fs->report.bad_header += (uint64_t)count;
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "no ubifs node at %u:%u", lnum, offs);
    }
    status = mtdf_src_get(fs, mtdf_f_pos(u, lnum, offs), len, NULL, &p);
    if (status != MTDF_OK)
        return status;
    if (mtdf_crc(fs, 0xffffffffu, p + 8, len - 8) != mtdf_le32(p + 4)) {
        fs->report.bad_node_crc += (uint64_t)count;
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs node at %u:%u fails its CRC", lnum, offs);
    }
    fs->report.nodes += (uint64_t)count;
    *out = p;
    *out_len = len;
    return MTDF_OK;
}

static int mtdf_f_skip_padding(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t lnum, uint32_t *offs)
{
    const uint8_t *p;
    int status;

    while (*offs < u->leb_size && mtdf_f_pos(u, lnum, *offs) < fs->len) {
        status = mtdf_src_get(fs, mtdf_f_pos(u, lnum, *offs), 1, NULL, &p);
        if (status != MTDF_OK)
            return status;
        if (*p != MTDF_F_PADDING_BYTE)
            break;
        (*offs)++;
    }
    *offs = (*offs + 7u) & ~7u;
    return MTDF_OK;
}

static int mtdf_f_next(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t lnum, uint32_t *offs, uint32_t *at,
                       const uint8_t **out, uint32_t *out_len)
{
    const uint8_t *p;
    uint32_t pad;
    int status;

    for (;;) {
        status = mtdf_f_skip_padding(fs, u, lnum, offs);
        if (status != MTDF_OK)
            return status;
        if (*offs > u->leb_size - MTDF_F_CH || mtdf_f_pos(u, lnum, *offs) > fs->len - MTDF_F_CH ||
            fs->len < MTDF_F_CH)
            return MTDF_END;
        status = mtdf_src_get(fs, mtdf_f_pos(u, lnum, *offs), MTDF_F_CH, NULL, &p);
        if (status != MTDF_OK)
            return status;
        if (mtdf_le32(p) == 0xffffffffu)
            return MTDF_END;
        *at = *offs;
        status = mtdf_f_node(fs, u, lnum, *offs, 1, out, out_len);
        if (status != MTDF_OK)
            return status;
        if ((*out)[20] != MTDF_F_T_PAD) {
            *offs += (*out_len + 7u) & ~7u;
            return MTDF_OK;
        }
        pad = *out_len < MTDF_F_PAD_LEN ? MTDF_NONE : mtdf_le32(*out + 24);
        if (pad > u->leb_size - *offs - MTDF_F_PAD_LEN)
            return fs->report.bad_structure++, mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs padding past its LEB");
        *offs += (MTDF_F_PAD_LEN + pad + 7u) & ~7u;
    }
}

static int mtdf_f_geometry(mtdf_fs_t *fs, mtdf_ubifs_t *u, const uint8_t *sb)
{
    uint32_t min_io = mtdf_le32(sb + 32);
    uint64_t lebs;

    u->leb_size = mtdf_le32(sb + 36);
    u->leb_cnt = mtdf_le32(sb + 40);
    u->log_lebs = mtdf_le32(sb + 56);
    u->fanout = mtdf_le32(sb + 72);
    lebs = (uint64_t)MTDF_F_LOG_LNUM + u->log_lebs + mtdf_le32(sb + 60) + mtdf_le32(sb + 64);
    if (u->leb_size < MTDF_F_MIN_LEB || u->leb_size > MTDF_F_MAX_LEB || u->leb_size % 8 != 0 || min_io < 8 ||
        min_io > u->leb_size || (min_io & (min_io - 1)) != 0)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs superblock: LEB size %u, I/O unit %u", u->leb_size, min_io);
    if (u->log_lebs < 2 || lebs >= u->leb_cnt)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs superblock: %u LEBs cannot hold the areas", u->leb_cnt);
    if (u->fanout < 3 || u->fanout > (MTDF_SMALL - MTDF_F_IDX_LEN) / MTDF_F_BRANCH)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs superblock: fanout %u", u->fanout);
    u->main_first = (uint32_t)lebs;
    return MTDF_OK;
}

static int mtdf_f_superblock(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    const uint8_t *p;
    uint32_t len;
    int status;

    if (fs->len < MTDF_F_SB_LEN)
        return MTDF_ERR_FORMAT;
    status = mtdf_src_get(fs, 0, MTDF_F_CH, NULL, &p);
    if (status != MTDF_OK)
        return status;
    if (mtdf_le32(p) != MTDF_F_MAGIC || p[20] != MTDF_F_T_SB)
        return MTDF_ERR_FORMAT;
    u->leb_size = MTDF_F_SB_LEN;
    u->leb_cnt = 1;
    status = mtdf_f_node(fs, u, 0, 0, 1, &p, &len);
    if (status != MTDF_OK)
        return status;
    if (len != MTDF_F_SB_LEN)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs superblock of %u bytes", len);
    if (p[27] != 0)
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "ubifs key format %u", p[27]);
    if (mtdf_le32(p + 80) == 0 || mtdf_le32(p + 80) > MTDF_F_MAX_FORMAT)
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "ubifs format version %u", mtdf_le32(p + 80));
    if (mtdf_le32(p + 28) & MTDF_F_FLG_AUTHENTICATION)
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "authenticated ubifs");
    return mtdf_f_geometry(fs, u, p);
}

static void mtdf_f_take_master(mtdf_ubifs_t *u, const uint8_t *p)
{
    u->cmt_no = mtdf_le64(p + 32);
    u->log_lnum = mtdf_le32(p + 44);
    u->root_lnum = mtdf_le32(p + 48);
    u->root_offs = mtdf_le32(p + 52);
    u->root_len = mtdf_le32(p + 56);
}

static int mtdf_f_master(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    const uint8_t *p;
    uint64_t best = 0;
    uint32_t lnum;
    uint32_t offs;
    uint32_t at;
    uint32_t len;
    int found = 0;
    int status;

    for (lnum = 1; lnum <= 2; lnum++) {
        offs = 0;
        while ((status = mtdf_f_next(fs, u, lnum, &offs, &at, &p, &len)) == MTDF_OK) {
            if (p[20] != MTDF_F_T_MST || len != MTDF_F_MST_LEN) {
                fs->report.unknown++;
            } else if (!found || mtdf_le64(p + 8) > best) {
                found = 1;
                best = mtdf_le64(p + 8);
                mtdf_f_take_master(u, p);
            }
        }
        if (status != MTDF_END && status != MTDF_ERR_CORRUPT)
            return status;
    }
    if (!found)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs: no valid master node");
    if (u->log_lnum < MTDF_F_LOG_LNUM || u->log_lnum - MTDF_F_LOG_LNUM >= u->log_lebs ||
        u->root_lnum < u->main_first || u->root_lnum >= u->leb_cnt || u->root_len < MTDF_F_IDX_LEN)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs master node points outside its areas");
    return MTDF_OK;
}

static int mtdf_f_add(mtdf_fs_t *fs, mtdf_ubifs_t *u, const mtdf_frec_t *rec)
{
    int status = mtdf_count(fs, 1);

    if (status == MTDF_OK && u->recs.len >= MTDF_NONE - 1)
        status = mtdf_fail(fs, MTDF_ERR_BUDGET, "more ubifs nodes than the index can address");
    if (status == MTDF_OK)
        status = mtdf_vec_grow(fs, &u->recs, sizeof(mtdf_frec_t), 1);
    if (status == MTDF_OK)
        *mtdf_f_rec(u, (uint32_t)u->recs.len++) = *rec;
    return status;
}

static int mtdf_f_inode_rec(mtdf_fs_t *fs, const uint8_t *p, uint32_t len, mtdf_frec_t *rec)
{
    if (len < MTDF_F_INO_LEN || mtdf_le32(p + 28) >> 29 != MTDF_F_K_INO || mtdf_le32(p + 112) > MTDF_F_MAX_INO_DATA ||
        mtdf_le32(p + 112) > len - MTDF_F_INO_LEN)
        return fs->report.bad_structure++, MTDF_ERR_CORRUPT;
    rec->kind = MTDF_F_INODE;
    rec->aux = mtdf_le32(p + 112);
    rec->a = mtdf_le32(p + 92);
    rec->b = mtdf_le32(p + 104);
    rec->c = mtdf_le32(p + 108);
    return MTDF_OK;
}

static int mtdf_f_data_rec(mtdf_fs_t *fs, const uint8_t *p, uint32_t len, mtdf_frec_t *rec)
{
    if (len < MTDF_F_DATA_LEN || mtdf_le32(p + 28) >> 29 != MTDF_F_K_DATA || mtdf_le32(p + 40) > MTDF_F_BLOCK)
        return fs->report.bad_structure++, MTDF_ERR_CORRUPT;
    rec->kind = MTDF_F_DATA;
    rec->aux = mtdf_le32(p + 28) & 0x1fffffffu;
    return MTDF_OK;
}

static int mtdf_f_dent_rec(mtdf_fs_t *fs, mtdf_ubifs_t *u, const uint8_t *p, uint32_t len, mtdf_frec_t *rec,
                           uint32_t kind)
{
    uint32_t nlen = mtdf_le16(p + 50);
    uint32_t key = kind == MTDF_F_DENTRY ? MTDF_F_K_DENT : MTDF_F_K_XENT;
    int status;

    if (len < MTDF_F_DENT_LEN || mtdf_le32(p + 28) >> 29 != key || nlen == 0 ||
        nlen >= len - MTDF_F_DENT_LEN || mtdf_le64(p + 40) > UINT32_MAX)
        return fs->report.bad_structure++, MTDF_ERR_CORRUPT;
    if (u->names.len > MTDF_NONE - nlen)
        return mtdf_fail(fs, MTDF_ERR_BUDGET, "ubifs names larger than the index can address");
    status = mtdf_vec_grow(fs, &u->names, 1, nlen);
    if (status != MTDF_OK)
        return status;
    memcpy((uint8_t *)u->names.data + u->names.len, p + MTDF_F_DENT_LEN, nlen);
    rec->kind = kind;
    rec->aux = mtdf_le64(p + 40);
    rec->a = (uint32_t)u->names.len;
    rec->b = nlen;
    u->names.len += nlen;
    return MTDF_OK;
}

static int mtdf_f_trun_rec(mtdf_fs_t *fs, const uint8_t *p, uint32_t len, mtdf_frec_t *rec)
{
    if (len < MTDF_F_TRUN_LEN)
        return fs->report.bad_structure++, MTDF_ERR_CORRUPT;
    rec->kind = MTDF_F_TRUNC;
    rec->aux = mtdf_le64(p + 48);
    return MTDF_OK;
}

static int mtdf_f_take(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t lnum, uint32_t offs, const uint8_t *p, uint32_t len,
                       const uint8_t *key)
{
    mtdf_frec_t rec;
    int status;

    memset(&rec, 0, sizeof(rec));
    rec.sqnum = mtdf_le64(p + 8);
    rec.inum = mtdf_le32(p + 24);
    rec.lnum = lnum;
    rec.offs = offs;
    rec.len = len;
    if (key != NULL && (p[20] > MTDF_F_T_XENT || memcmp(p + 24, key, 8) != 0))
        return fs->report.bad_structure++, MTDF_OK;
    switch (p[20]) {
    case MTDF_F_T_INO:
        status = mtdf_f_inode_rec(fs, p, len, &rec);
        break;
    case MTDF_F_T_DATA:
        status = mtdf_f_data_rec(fs, p, len, &rec);
        break;
    case MTDF_F_T_DENT:
        status = mtdf_f_dent_rec(fs, u, p, len, &rec, MTDF_F_DENTRY);
        break;
    case MTDF_F_T_XENT:
        status = mtdf_f_dent_rec(fs, u, p, len, &rec, MTDF_F_XENTRY);
        break;
    case MTDF_F_T_TRUN:
        status = mtdf_f_trun_rec(fs, p, len, &rec);
        break;
    default:
        fs->report.unknown++;
        return MTDF_OK;
    }
    if (status == MTDF_ERR_CORRUPT)
        return MTDF_OK;
    return status == MTDF_OK ? mtdf_f_add(fs, u, &rec) : status;
}

static int mtdf_f_push(mtdf_fs_t *fs, mtdf_ubifs_t *u, const uint8_t *branch, uint32_t level)
{
    mtdf_fbranch_t *b;
    uint32_t lnum = mtdf_le32(branch);
    uint32_t offs = mtdf_le32(branch + 4);
    uint32_t len = mtdf_le32(branch + 8);
    int status;

    if (lnum < u->main_first || lnum >= u->leb_cnt || offs % 8 != 0 || offs >= u->leb_size ||
        len < MTDF_F_CH || len > u->leb_size - offs)
        return fs->report.bad_structure++, MTDF_OK;
    status = mtdf_vec_grow(fs, &u->stack, sizeof(mtdf_fbranch_t), 1);
    if (status != MTDF_OK)
        return status;
    b = (mtdf_fbranch_t *)u->stack.data + u->stack.len++;
    b->lnum = lnum;
    b->offs = offs;
    b->len = len;
    b->level = level;
    memcpy(b->key, branch + 12, 8);
    return MTDF_OK;
}

static int mtdf_f_idx(mtdf_fs_t *fs, mtdf_ubifs_t *u, const mtdf_fbranch_t *b)
{
    const uint8_t *p;
    uint32_t len;
    uint32_t count;
    uint32_t level;
    uint32_t i;
    int status = mtdf_count(fs, 1);

    if (status == MTDF_OK)
        status = mtdf_f_node(fs, u, b->lnum, b->offs, 1, &p, &len);
    if (status == MTDF_ERR_CORRUPT && b->level != MTDF_NONE)
        return MTDF_OK;
    if (status != MTDF_OK)
        return status;
    count = mtdf_le16(p + 24);
    level = mtdf_le16(p + 26);
    if (p[20] != MTDF_F_T_IDX || len != b->len || count == 0 || count > u->fanout ||
        len != MTDF_F_IDX_LEN + count * MTDF_F_BRANCH || level > MTDF_F_MAX_LEVEL ||
        (b->level != MTDF_NONE && level != b->level)) {
        fs->report.bad_structure++;
        return b->level == MTDF_NONE ? mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs index root damaged") : MTDF_OK;
    }
    for (i = 0; i < count && status == MTDF_OK; i++)
        status = mtdf_f_push(fs, u, p + MTDF_F_IDX_LEN + i * MTDF_F_BRANCH, level == 0 ? MTDF_F_LEAF : level - 1);
    return status;
}

static int mtdf_f_leaf(mtdf_fs_t *fs, mtdf_ubifs_t *u, const mtdf_fbranch_t *b)
{
    const uint8_t *p;
    uint32_t len = b->len;
    int status;

    status = mtdf_src_get(fs, mtdf_f_pos(u, b->lnum, b->offs), len < MTDF_F_DATA_LEN ? MTDF_F_CH : MTDF_F_DATA_LEN,
                          NULL, &p);
    if (status == MTDF_ERR_CORRUPT)
        return fs->report.bad_structure++, MTDF_OK;
    if (status != MTDF_OK)
        return status;
    if (mtdf_le32(p) != MTDF_F_MAGIC || mtdf_le32(p + 16) != len)
        return fs->report.bad_header++, MTDF_OK;
    if (p[20] == MTDF_F_T_DATA) {
        if (len < MTDF_F_DATA_LEN)
            return fs->report.bad_header++, MTDF_OK;
        fs->report.nodes++;
        return mtdf_f_take(fs, u, b->lnum, b->offs, p, len, b->key);
    }
    status = mtdf_f_node(fs, u, b->lnum, b->offs, 1, &p, &len);
    if (status == MTDF_ERR_CORRUPT)
        return MTDF_OK;
    return status == MTDF_OK ? mtdf_f_take(fs, u, b->lnum, b->offs, p, len, b->key) : status;
}

static int mtdf_f_index(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    mtdf_fbranch_t b;
    int status = MTDF_OK;

    memset(&b, 0, sizeof(b));
    b.lnum = u->root_lnum;
    b.offs = u->root_offs;
    b.len = u->root_len;
    b.level = MTDF_NONE;
    status = mtdf_f_idx(fs, u, &b);
    while (status == MTDF_OK && u->stack.len > 0) {
        b = ((mtdf_fbranch_t *)u->stack.data)[--u->stack.len];
        status = b.level == MTDF_F_LEAF ? mtdf_f_leaf(fs, u, &b) : mtdf_f_idx(fs, u, &b);
    }
    return status;
}

static int mtdf_f_add_bud(mtdf_fs_t *fs, mtdf_ubifs_t *u, const uint8_t *p, uint32_t len)
{
    uint32_t lnum = mtdf_le32(p + 24);
    uint32_t offs = mtdf_le32(p + 28);
    int status;

    if (len < MTDF_F_REF_LEN || lnum < u->main_first || lnum >= u->leb_cnt || offs >= u->leb_size || offs % 8 != 0)
        return fs->report.bad_structure++, MTDF_OK;
    status = mtdf_vec_grow(fs, &u->buds, sizeof(uint32_t), 2);
    if (status != MTDF_OK)
        return status;
    ((uint32_t *)u->buds.data)[u->buds.len++] = lnum;
    ((uint32_t *)u->buds.data)[u->buds.len++] = offs;
    return MTDF_OK;
}

static int mtdf_f_log_node(mtdf_fs_t *fs, mtdf_ubifs_t *u, int first, const uint8_t *p, uint32_t len,
                           uint64_t *cs_sqnum)
{
    uint64_t sqnum = mtdf_le64(p + 8);

    if (*cs_sqnum == 0) {
        if (p[20] != MTDF_F_T_CS || len < MTDF_F_CS_LEN || mtdf_le64(p + 24) != u->cmt_no)
            return fs->report.bad_structure++, MTDF_END;
        *cs_sqnum = sqnum;
        return MTDF_OK;
    }
    if ((first && sqnum <= *cs_sqnum) || p[20] == MTDF_F_T_CS)
        return MTDF_END;
    if (p[20] == MTDF_F_T_REF)
        return mtdf_f_add_bud(fs, u, p, len);
    fs->report.unknown++;
    return MTDF_OK;
}

static int mtdf_f_log(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    const uint8_t *p;
    uint64_t cs_sqnum = 0;
    uint32_t n;
    uint32_t lnum;
    uint32_t offs;
    uint32_t at;
    uint32_t len;
    int first;
    int status = MTDF_OK;

    for (n = 0; n < u->log_lebs && status == MTDF_OK; n++) {
        lnum = MTDF_F_LOG_LNUM + (u->log_lnum - MTDF_F_LOG_LNUM + n) % u->log_lebs;
        offs = 0;
        first = 1;
        while (status == MTDF_OK && (status = mtdf_f_next(fs, u, lnum, &offs, &at, &p, &len)) == MTDF_OK) {
            status = mtdf_f_log_node(fs, u, first, p, len, &cs_sqnum);
            first = 0;
        }
        if (status == MTDF_END && !first && cs_sqnum != 0 && offs < u->leb_size)
            break;
        if (status == MTDF_END && !first && cs_sqnum != 0)
            status = MTDF_OK;
    }
    return status == MTDF_END || status == MTDF_ERR_CORRUPT ? MTDF_OK : status;
}

static int mtdf_f_bud(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t lnum, uint32_t offs)
{
    const uint8_t *p;
    uint32_t at;
    uint32_t len;
    int status;

    while ((status = mtdf_f_next(fs, u, lnum, &offs, &at, &p, &len)) == MTDF_OK) {
        status = mtdf_f_take(fs, u, lnum, at, p, len, NULL);
        if (status != MTDF_OK)
            return status;
    }
    return status == MTDF_END || status == MTDF_ERR_CORRUPT ? MTDF_OK : status;
}

static int mtdf_f_journal(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    const uint32_t *buds;
    size_t i;
    int status = mtdf_f_log(fs, u);

    for (i = 0; status == MTDF_OK && i < u->buds.len; i += 2) {
        buds = u->buds.data;
        status = mtdf_f_bud(fs, u, buds[i], buds[i + 1]);
    }
    return status;
}

static int mtdf_f_name_order(const mtdf_ubifs_t *u, const mtdf_frec_t *a, const mtdf_frec_t *b)
{
    const uint8_t *names = u->names.data;
    uint32_t common = a->b < b->b ? a->b : b->b;
    int order = memcmp(names + a->a, names + b->a, common);

    if (order != 0)
        return order;
    return a->b < b->b ? -1 : a->b > b->b;
}

static int mtdf_f_named(uint32_t kind)
{
    return kind == MTDF_F_DENTRY || kind == MTDF_F_XENTRY;
}

static int mtdf_f_order(const void *ctx, const void *pa, const void *pb)
{
    const mtdf_frec_t *a = pa;
    const mtdf_frec_t *b = pb;
    int order;

    if (a->inum != b->inum)
        return a->inum < b->inum ? -1 : 1;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->kind == MTDF_F_DATA && a->aux != b->aux)
        return a->aux < b->aux ? -1 : 1;
    if (mtdf_f_named(a->kind) && (order = mtdf_f_name_order(ctx, a, b)) != 0)
        return order;
    return (a->sqnum > b->sqnum) - (a->sqnum < b->sqnum);
}

static size_t mtdf_f_key_end(const mtdf_ubifs_t *u, size_t first, size_t end)
{
    const mtdf_frec_t *a = mtdf_f_rec(u, (uint32_t)first);
    const mtdf_frec_t *b;
    size_t i = first;

    while (i + 1 < end) {
        b = mtdf_f_rec(u, (uint32_t)(i + 1));
        if (b->kind != a->kind || (a->kind == MTDF_F_DATA && b->aux != a->aux) ||
            (mtdf_f_named(a->kind) && mtdf_f_name_order(u, a, b) != 0))
            break;
        i++;
    }
    return i;
}

static void mtdf_f_least_sizes(mtdf_ubifs_t *u, size_t first, size_t end)
{
    uint64_t least = UINT64_MAX;
    size_t i;

    for (i = end; i > first; i--) {
        if (mtdf_f_rec(u, (uint32_t)(i - 1))->aux < least)
            least = mtdf_f_rec(u, (uint32_t)(i - 1))->aux;
        mtdf_f_rec(u, (uint32_t)(i - 1))->aux = least;
    }
}

static int mtdf_f_truncated(const mtdf_ubifs_t *u, size_t first, size_t end, const mtdf_frec_t *data)
{
    size_t lo = first;
    size_t hi = end;
    size_t mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (mtdf_f_rec(u, (uint32_t)mid)->sqnum > data->sqnum)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo < end && mtdf_f_rec(u, (uint32_t)lo)->aux <= data->aux * MTDF_F_BLOCK;
}

static int mtdf_f_push32(mtdf_fs_t *fs, mtdf_vec_t *vec, uint32_t value)
{
    int status = mtdf_vec_grow(fs, vec, sizeof(uint32_t), 1);

    if (status == MTDF_OK)
        ((uint32_t *)vec->data)[vec->len++] = value;
    return status;
}

static int mtdf_f_live_inode(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t rec, uint32_t *out_live)
{
    mtdf_fino_t *ino;
    int status = mtdf_vec_grow(fs, &u->inodes, sizeof(mtdf_fino_t), 1);

    if (status != MTDF_OK)
        return status;
    *out_live = (uint32_t)u->inodes.len++;
    ino = mtdf_f_ino(u, *out_live);
    ino->inum = mtdf_f_rec(u, rec)->inum;
    ino->rec = rec;
    ino->data_first = (uint32_t)u->data.len;
    ino->data_count = 0;
    ino->dir_dent = MTDF_NONE;
    ino->used = 0;
    return MTDF_OK;
}

static int mtdf_f_live_dent(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t rec)
{
    mtdf_fdent_t *d;
    int status = mtdf_vec_grow(fs, &u->dents, sizeof(mtdf_fdent_t), 1);

    if (status != MTDF_OK)
        return status;
    d = mtdf_f_dent(u, (uint32_t)u->dents.len++);
    d->rec = rec;
    d->entry = MTDF_NONE;
    d->state = MTDF_F_NEW;
    return MTDF_OK;
}

static int mtdf_f_contents(mtdf_fs_t *fs, mtdf_ubifs_t *u, size_t i, size_t end, size_t trun, uint32_t live,
                           int deleted)
{
    size_t last;
    uint64_t lost = 0;
    int status = MTDF_OK;

    for (; i < end && status == MTDF_OK; i = last + 1) {
        last = mtdf_f_key_end(u, i, end);
        fs->report.superseded += last - i;
        if (mtdf_f_rec(u, (uint32_t)last)->kind == MTDF_F_DENTRY) {
            if (mtdf_f_rec(u, (uint32_t)last)->aux != 0)
                status = mtdf_f_live_dent(fs, u, (uint32_t)last);
        } else if (mtdf_f_rec(u, (uint32_t)last)->kind == MTDF_F_XENTRY) {
            lost += live == MTDF_NONE;
            if (live != MTDF_NONE && mtdf_f_rec(u, (uint32_t)last)->aux != 0)
                status = mtdf_f_push32(fs, &u->xents, (uint32_t)last);
        } else if (!mtdf_f_truncated(u, trun, i, mtdf_f_rec(u, (uint32_t)last))) {
            lost += live == MTDF_NONE;
            if (live != MTDF_NONE && (status = mtdf_f_push32(fs, &u->data, (uint32_t)last)) == MTDF_OK)
                mtdf_f_ino(u, live)->data_count++;
        }
    }
    if (deleted)
        fs->report.superseded += lost;
    else
        fs->report.orphans += lost != 0;
    return status;
}

static int mtdf_f_group(mtdf_fs_t *fs, mtdf_ubifs_t *u, size_t first, size_t end)
{
    uint32_t live = MTDF_NONE;
    size_t i = first;
    size_t trun;
    int status = MTDF_OK;

    while (i < end && mtdf_f_rec(u, (uint32_t)i)->kind == MTDF_F_INODE)
        i++;
    if (i > first) {
        fs->report.superseded += i - first - 1;
        if (mtdf_f_rec(u, (uint32_t)(i - 1))->a > 0)
            status = mtdf_f_live_inode(fs, u, (uint32_t)(i - 1), &live);
    }
    trun = i;
    while (i < end && mtdf_f_rec(u, (uint32_t)i)->kind == MTDF_F_TRUNC)
        i++;
    mtdf_f_least_sizes(u, trun, i);
    return status == MTDF_OK ? mtdf_f_contents(fs, u, i, end, trun, live, trun > first && live == MTDF_NONE) : status;
}

static int mtdf_f_replay(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    mtdf_frec_t *tmp;
    size_t i;
    size_t end;
    int status = MTDF_OK;

    if (u->recs.len > SIZE_MAX / sizeof(mtdf_frec_t))
        return MTDF_ERR_NOMEM;
    tmp = mtdf_alloc(fs, u->recs.len * sizeof(mtdf_frec_t));
    if (tmp == NULL)
        return MTDF_ERR_NOMEM;
    mtdf_sort(u->recs.data, tmp, u->recs.len, sizeof(mtdf_frec_t), mtdf_f_order, u);
    mtdf_free(fs, tmp);
    for (i = 0; i < u->recs.len && status == MTDF_OK; i = end) {
        end = i + 1;
        while (end < u->recs.len && mtdf_f_rec(u, (uint32_t)end)->inum == mtdf_f_rec(u, (uint32_t)i)->inum)
            end++;
        status = mtdf_f_group(fs, u, i, end);
    }
    return status;
}

static uint32_t mtdf_f_find(const mtdf_ubifs_t *u, uint64_t inum)
{
    uint32_t lo = 0;
    uint32_t hi = (uint32_t)u->inodes.len;
    uint32_t mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (mtdf_f_ino(u, mid)->inum == inum)
            return mid;
        if (mtdf_f_ino(u, mid)->inum < inum)
            lo = mid + 1;
        else
            hi = mid;
    }
    return MTDF_NONE;
}

static int mtdf_f_is_dir(const mtdf_ubifs_t *u, uint32_t ino)
{
    return (mtdf_f_rec(u, mtdf_f_ino(u, ino)->rec)->b & MTDF_S_IFMT) == MTDF_S_IFDIR;
}

static void mtdf_f_dirs(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    const mtdf_frec_t *rec;
    mtdf_fdent_t *d;
    uint32_t target;
    uint32_t i;

    for (i = 0; i < u->dents.len; i++) {
        d = mtdf_f_dent(u, i);
        rec = mtdf_f_rec(u, d->rec);
        target = mtdf_f_find(u, rec->aux);
        if (!mtdf_name_ok((const char *)u->names.data + rec->a, rec->b)) {
            fs->report.bad_name++;
            d->state = MTDF_F_DROPPED;
        } else if (target == MTDF_NONE || rec->aux == MTDF_F_ROOT_INUM ||
                   (mtdf_f_is_dir(u, target) && mtdf_f_ino(u, target)->dir_dent != MTDF_NONE)) {
            fs->report.bad_structure++;
            d->state = MTDF_F_DROPPED;
        } else if (mtdf_f_is_dir(u, target)) {
            mtdf_f_ino(u, target)->dir_dent = i;
        }
    }
}

static int mtdf_f_resolve(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t start)
{
    uint32_t cur = start;
    uint32_t parent;
    uint32_t outcome = MTDF_F_DROPPED;
    uint64_t *reason = &fs->report.orphans;
    mtdf_fdent_t *d;
    int status;

    u->chain.len = 0;
    for (;;) {
        d = mtdf_f_dent(u, cur);
        if (d->state == MTDF_F_PLACED || d->state == MTDF_F_DROPPED) {
            outcome = d->state;
            break;
        }
        if (d->state == MTDF_F_WALKING) {
            reason = &fs->report.loops;
            break;
        }
        status = mtdf_f_push32(fs, &u->chain, cur);
        if (status != MTDF_OK)
            return status;
        d->state = MTDF_F_WALKING;
        if (mtdf_f_rec(u, d->rec)->inum == MTDF_F_ROOT_INUM) {
            outcome = MTDF_F_PLACED;
            break;
        }
        parent = mtdf_f_find(u, mtdf_f_rec(u, d->rec)->inum);
        if (parent == MTDF_NONE || mtdf_f_ino(u, parent)->dir_dent == MTDF_NONE)
            break;
        cur = mtdf_f_ino(u, parent)->dir_dent;
    }
    while (u->chain.len > 0) {
        mtdf_f_dent(u, ((uint32_t *)u->chain.data)[--u->chain.len])->state = outcome;
        if (outcome == MTDF_F_DROPPED)
            (*reason)++;
    }
    return MTDF_OK;
}

static int mtdf_f_attributes(mtdf_fs_t *fs, mtdf_ubifs_t *u, uint32_t ino, mtdf_entry_t *entry)
{
    const mtdf_frec_t *rec = mtdf_f_rec(u, mtdf_f_ino(u, ino)->rec);
    const uint8_t *p;
    uint32_t len;
    uint32_t dev;
    int status;

    status = mtdf_f_node(fs, u, rec->lnum, rec->offs, 0, &p, &len);
    if (status != MTDF_OK)
        return status;
    if (len < MTDF_F_INO_LEN || mtdf_le32(p + 112) > len - MTDF_F_INO_LEN)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs inode changed while reading");
    entry->ref = ino;
    entry->mode = mtdf_le32(p + 104);
    entry->uid = mtdf_le32(p + 96);
    entry->gid = mtdf_le32(p + 100);
    entry->mtime = mtdf_f_time(mtdf_le64(p + 72));
    entry->size = (entry->mode & MTDF_S_IFMT) == MTDF_S_IFLNK ? mtdf_le32(p + 112) : mtdf_le64(p + 48);
    if ((entry->mode & MTDF_S_IFMT) == MTDF_S_IFCHR || (entry->mode & MTDF_S_IFMT) == MTDF_S_IFBLK) {
        dev = mtdf_le32(p + 112) >= 4 ? mtdf_le32(p + MTDF_F_INO_LEN) : 0;
        entry->dev_major = (dev & 0xfff00u) >> 8;
        entry->dev_minor = (dev & 0xffu) | ((dev >> 12) & 0xfff00u);
    }
    return MTDF_OK;
}

static int mtdf_f_place(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    const mtdf_frec_t *rec;
    mtdf_fdent_t *d;
    uint32_t target;
    uint32_t i;
    int status = MTDF_OK;

    for (i = 0; i < u->dents.len && status == MTDF_OK; i++) {
        d = mtdf_f_dent(u, i);
        rec = mtdf_f_rec(u, d->rec);
        if (d->state != MTDF_F_PLACED)
            continue;
        status = mtdf_tree_add(fs, (const char *)u->names.data + rec->a, rec->b, &d->entry);
        target = mtdf_f_find(u, rec->aux);
        if (status == MTDF_OK)
            status = mtdf_f_attributes(fs, u, target, mtdf_entry(fs, d->entry));
        mtdf_f_ino(u, target)->used++;
    }
    for (i = 0; i < u->dents.len && status == MTDF_OK; i++) {
        d = mtdf_f_dent(u, i);
        rec = mtdf_f_rec(u, d->rec);
        if (d->state == MTDF_F_PLACED && rec->inum != MTDF_F_ROOT_INUM)
            mtdf_entry(fs, d->entry)->parent = mtdf_f_dent(u, mtdf_f_ino(u, mtdf_f_find(u, rec->inum))->dir_dent)->entry;
    }
    return status;
}

static int mtdf_f_finish(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    mtdf_entry_t *entry;
    uint32_t root = mtdf_f_find(u, MTDF_F_ROOT_INUM);
    uint32_t i;
    int status = MTDF_OK;

    for (i = 1; i < mtdf_entry_count(fs); i++) {
        entry = mtdf_entry(fs, i);
        entry->nlink = mtdf_f_ino(u, (uint32_t)entry->ref)->used;
    }
    for (i = 0; i < u->inodes.len; i++) {
        if (mtdf_f_ino(u, i)->used == 0 && mtdf_f_ino(u, i)->inum != MTDF_F_ROOT_INUM &&
            !(mtdf_f_rec(u, mtdf_f_ino(u, i)->rec)->c & MTDF_F_INO_XATTR))
            fs->report.orphans++;
    }
    if (root == MTDF_NONE || !mtdf_f_is_dir(u, root))
        return fs->report.bad_structure++, MTDF_OK;
    status = mtdf_f_attributes(fs, u, root, mtdf_entry(fs, MTDF_ROOT));
    mtdf_entry(fs, MTDF_ROOT)->mode = MTDF_S_IFDIR | (mtdf_entry(fs, MTDF_ROOT)->mode & 07777);
    return status;
}

static int mtdf_f_xattrs(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    const mtdf_frec_t *rec;
    const mtdf_frec_t *target;
    mtdf_xrec_t x;
    uint32_t at;
    uint32_t i;
    int status = MTDF_OK;

    memset(&x, 0, sizeof(x));
    for (i = 0; i < u->xents.len && status == MTDF_OK; i++) {
        rec = mtdf_f_rec(u, ((uint32_t *)u->xents.data)[i]);
        at = mtdf_f_find(u, rec->aux);
        target = at == MTDF_NONE ? NULL : mtdf_f_rec(u, mtdf_f_ino(u, at)->rec);
        if (target == NULL || !(target->c & MTDF_F_INO_XATTR)) {
            fs->report.bad_structure++;
            continue;
        }
        x.owner = mtdf_f_find(u, rec->inum);
        x.ref = at;
        x.seq = rec->sqnum;
        x.value_len = (uint32_t)target->aux;
        status = mtdf_xattr_add(fs, &x, "", (const char *)u->names.data + rec->a, rec->b);
    }
    return status == MTDF_OK ? mtdf_xattr_finish(fs) : status;
}

static int mtdf_f_build(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    uint32_t i;
    int status = mtdf_f_replay(fs, u);

    if (status == MTDF_OK)
        mtdf_f_dirs(fs, u);
    for (i = 0; status == MTDF_OK && i < u->dents.len; i++)
        status = mtdf_f_resolve(fs, u, i);
    if (status == MTDF_OK)
        status = mtdf_f_place(fs, u);
    if (status == MTDF_OK)
        status = mtdf_f_xattrs(fs, u);
    return status == MTDF_OK ? mtdf_f_finish(fs, u) : status;
}

static int mtdf_f_unpack(mtdf_fs_t *fs, const uint8_t *p, uint32_t len, uint32_t size, uint8_t *out)
{
    const uint8_t *in = p + MTDF_F_DATA_LEN;
    size_t in_len = len - MTDF_F_DATA_LEN;

    switch (mtdf_le16(p + 44)) {
    case MTDF_F_COMPR_NONE:
        if (in_len != size)
            return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs plain block of the wrong size");
        memcpy(out, in, size);
        return MTDF_OK;
    case MTDF_F_COMPR_LZO:
        return mtdf_unlzo(fs, in, in_len, out, size);
    case MTDF_F_COMPR_ZLIB:
        return mtdf_inflate(fs, in, in_len, out, size, 1);
    case MTDF_F_COMPR_ZSTD:
        return mtdf_unzstd(fs, in, in_len, out, size);
    default:
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "ubifs compression %u", mtdf_le16(p + 44));
    }
}

static int mtdf_f_decode(mtdf_fs_t *fs, mtdf_ubifs_t *u, const mtdf_frec_t *rec, uint8_t *out)
{
    const uint8_t *p;
    uint32_t len;
    uint32_t size;
    int status;

    status = mtdf_f_node(fs, u, rec->lnum, rec->offs, 0, &p, &len);
    if (status != MTDF_OK)
        return status;
    size = mtdf_le32(p + 40);
    if (p[20] != MTDF_F_T_DATA || len < MTDF_F_DATA_LEN || size > MTDF_F_BLOCK)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs data node changed while reading");
    if (mtdf_le16(p + 46) != 0)
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "encrypted ubifs data");
    status = mtdf_charge(fs, size);
    if (status == MTDF_OK && size > 0)
        status = mtdf_f_unpack(fs, p, len, size, out);
    if (status == MTDF_OK)
        memset(out + size, 0, MTDF_F_BLOCK - size);
    return status;
}

static uint32_t mtdf_f_find_block(const mtdf_ubifs_t *u, const mtdf_fino_t *ino, uint64_t block)
{
    uint32_t lo = ino->data_first;
    uint32_t hi = ino->data_first + ino->data_count;
    uint32_t mid;
    uint64_t at;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        at = mtdf_f_rec(u, mtdf_f_data(u, mid))->aux;
        if (at == block)
            return mtdf_f_data(u, mid);
        if (at < block)
            lo = mid + 1;
        else
            hi = mid;
    }
    return MTDF_NONE;
}

static int mtdf_f_block(mtdf_fs_t *fs, mtdf_ubifs_t *u, const mtdf_fino_t *ino, uint64_t block, const uint8_t **out)
{
    uint32_t rec = mtdf_f_find_block(u, ino, block);
    int status = mtdf_unit_buffer(fs);

    if (status != MTDF_OK)
        return status;
    *out = fs->unit;
    if (fs->unit_valid && fs->unit_key == (uint64_t)rec + 1 && rec != MTDF_NONE)
        return MTDF_OK;
    fs->unit_valid = 0;
    if (rec == MTDF_NONE) {
        memset(fs->unit, 0, MTDF_F_BLOCK);
        return MTDF_OK;
    }
    status = mtdf_f_decode(fs, u, mtdf_f_rec(u, rec), fs->unit);
    if (status != MTDF_OK)
        return status;
    fs->unit_valid = 1;
    fs->unit_key = (uint64_t)rec + 1;
    fs->unit_key2 = 0;
    return MTDF_OK;
}

static int mtdf_f_link(mtdf_fs_t *fs, mtdf_ubifs_t *u, const mtdf_fino_t *ino, uint64_t off, uint8_t *buf, size_t len)
{
    const mtdf_frec_t *rec = mtdf_f_rec(u, ino->rec);
    const uint8_t *p;
    uint32_t node_len;
    int status;

    status = mtdf_f_node(fs, u, rec->lnum, rec->offs, 0, &p, &node_len);
    if (status != MTDF_OK)
        return status;
    if (node_len < MTDF_F_INO_LEN || mtdf_le32(p + 112) > node_len - MTDF_F_INO_LEN || off > mtdf_le32(p + 112) ||
        len > mtdf_le32(p + 112) - off)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs link inode changed while reading");
    memcpy(buf, p + MTDF_F_INO_LEN + off, len);
    return MTDF_OK;
}

static int mtdf_f_xattr(mtdf_fs_t *fs, const mtdf_xrec_t *x, uint8_t *buf)
{
    mtdf_ubifs_t *u = mtdf_f(fs);
    const mtdf_frec_t *rec = mtdf_f_rec(u, mtdf_f_ino(u, (uint32_t)x->ref)->rec);
    const uint8_t *p;
    uint32_t len;
    int status;

    status = mtdf_f_node(fs, u, rec->lnum, rec->offs, 0, &p, &len);
    if (status != MTDF_OK)
        return status;
    if (len < MTDF_F_INO_LEN || mtdf_le32(p + 112) != x->value_len || x->value_len > len - MTDF_F_INO_LEN)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "ubifs xattr inode changed while reading");
    memcpy(buf, p + MTDF_F_INO_LEN, x->value_len);
    return MTDF_OK;
}

static int mtdf_f_read(mtdf_fs_t *fs, const mtdf_entry_t *entry, uint64_t off, uint8_t *buf, size_t len)
{
    mtdf_ubifs_t *u = mtdf_f(fs);
    const mtdf_fino_t *ino = mtdf_f_ino(u, (uint32_t)entry->ref);
    const uint8_t *block;
    size_t inner;
    size_t piece;
    int status;

    if (mtdf_f_rec(u, ino->rec)->c & MTDF_F_INO_CRYPT)
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "encrypted ubifs file");
    if ((entry->mode & MTDF_S_IFMT) == MTDF_S_IFLNK)
        return mtdf_f_link(fs, u, ino, off, buf, len);
    while (len > 0) {
        inner = (size_t)(off % MTDF_F_BLOCK);
        piece = MTDF_F_BLOCK - inner < len ? MTDF_F_BLOCK - inner : len;
        status = mtdf_f_block(fs, u, ino, off / MTDF_F_BLOCK, &block);
        if (status != MTDF_OK)
            return status;
        memcpy(buf, block + inner, piece);
        buf += piece;
        off += piece;
        len -= piece;
    }
    return MTDF_OK;
}

static void mtdf_f_release(mtdf_fs_t *fs, mtdf_ubifs_t *u)
{
    mtdf_vec_free(fs, &u->names);
    mtdf_vec_free(fs, &u->dents);
    mtdf_vec_free(fs, &u->xents);
    mtdf_vec_free(fs, &u->stack);
    mtdf_vec_free(fs, &u->buds);
    mtdf_vec_free(fs, &u->chain);
}

static void mtdf_f_destroy(mtdf_fs_t *fs)
{
    mtdf_ubifs_t *u = mtdf_f(fs);

    mtdf_f_release(fs, u);
    mtdf_vec_free(fs, &u->recs);
    mtdf_vec_free(fs, &u->inodes);
    mtdf_vec_free(fs, &u->data);
    mtdf_free(fs, u);
    fs->state = NULL;
}

static const mtdf_driver_t mtdf_ubifs_driver = {mtdf_f_read, mtdf_f_destroy, mtdf_f_xattr};

int mtdf_ubifs_open(mtdf_fs_t *fs, int forced)
{
    mtdf_ubifs_t probe;
    mtdf_ubifs_t *u;
    uint64_t span;
    int status;

    (void)forced;
    memset(&probe, 0, sizeof(probe));
    status = mtdf_f_superblock(fs, &probe);
    if (status != MTDF_OK)
        return status;
    u = mtdf_alloc(fs, sizeof(*u));
    if (u == NULL)
        return MTDF_ERR_NOMEM;
    *u = probe;
    fs->state = u;
    fs->driver = &mtdf_ubifs_driver;
    span = (uint64_t)u->leb_cnt * u->leb_size;
    fs->info.format = MTDF_FORMAT_UBIFS;
    fs->info.image_size = span < fs->len ? span : fs->len;
    fs->info.block_size = u->leb_size;
    fs->info.unit_size = MTDF_F_BLOCK;
    status = mtdf_f_master(fs, u);
    if (status == MTDF_OK)
        status = mtdf_f_index(fs, u);
    if (status == MTDF_OK)
        status = mtdf_f_journal(fs, u);
    if (status == MTDF_OK)
        status = mtdf_f_build(fs, u);
    mtdf_f_release(fs, u);
    return status;
}
