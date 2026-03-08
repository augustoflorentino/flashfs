#include "flashfs_int.h"

#define MTDF_C_MAGIC 0x28cd3d45u
#define MTDF_C_SUPER 76u
#define MTDF_C_CRC_OFFSET 32u
#define MTDF_C_SHIFTED 512u
#define MTDF_C_PAGE 4096u
#define MTDF_C_FLAG_FSID_V2 0x1u
#define MTDF_C_FLAG_EXT_BLOCK_POINTERS 0x800u
#define MTDF_C_SUPPORTED_FLAGS (0xffu | 0x100u | 0x200u | 0x400u | 0x800u)
#define MTDF_C_BLOCK_UNCOMPRESSED 0x80000000u
#define MTDF_C_BLOCK_DIRECT 0x40000000u

static const uint8_t mtdf_c_signature[16] = {'C', 'o', 'm', 'p', 'r', 'e', 's', 's',
                                             'e', 'd', ' ', 'R', 'O', 'M', 'F', 'S'};

typedef struct mtdf_cinode {
    uint32_t mode;
    uint32_t uid;
    uint32_t size;
    uint32_t gid;
    uint32_t namelen;
    uint32_t offset;
} mtdf_cinode_t;

typedef struct mtdf_cramfs {
    int big;
    uint64_t base;
    uint64_t size;
    uint32_t flags;
    mtdf_vec_t seen;
    mtdf_vec_t stack;
} mtdf_cramfs_t;

typedef struct mtdf_cwalk {
    uint32_t entry;
    uint32_t offset;
    uint32_t size;
} mtdf_cwalk_t;

static mtdf_cramfs_t *mtdf_c(mtdf_fs_t *fs)
{
    return (mtdf_cramfs_t *)fs->state;
}

static void mtdf_c_inode(const uint8_t *p, int big, mtdf_cinode_t *out)
{
    uint32_t first = mtdf_get32(p, big);
    uint32_t second = mtdf_get32(p + 4, big);
    uint32_t third = mtdf_get32(p + 8, big);

    if (!big) {
        out->mode = first & 0xffffu;
        out->uid = first >> 16;
        out->size = second & 0xffffffu;
        out->gid = second >> 24;
        out->namelen = (third & 0x3fu) * 4;
        out->offset = (third >> 6) * 4;
        return;
    }
    out->mode = first >> 16;
    out->uid = first & 0xffffu;
    out->size = second >> 8;
    out->gid = second & 0xffu;
    out->namelen = (third >> 26) * 4;
    out->offset = (third & 0x3ffffffu) * 4;
}

static int mtdf_c_superblock(mtdf_fs_t *fs, uint64_t base, mtdf_cramfs_t *c, mtdf_cinode_t *root)
{
    const uint8_t *sb;
    uint32_t size;

    if (mtdf_src_get(fs, base, MTDF_C_SUPER, NULL, &sb) != MTDF_OK)
        return MTDF_ERR_FORMAT;
    if (mtdf_le32(sb) == MTDF_C_MAGIC)
        c->big = 0;
    else if (mtdf_be32(sb) == MTDF_C_MAGIC)
        c->big = 1;
    else
        return MTDF_ERR_FORMAT;
    if (memcmp(sb + 16, mtdf_c_signature, sizeof(mtdf_c_signature)) != 0)
        return MTDF_ERR_FORMAT;
    size = mtdf_get32(sb + 4, c->big);
    c->flags = mtdf_get32(sb + 8, c->big);
    mtdf_c_inode(sb + 64, c->big, root);
    if (c->flags & ~MTDF_C_SUPPORTED_FLAGS)
        return mtdf_fail(fs, MTDF_ERR_FORMAT, "cramfs flags 0x%x not supported", c->flags);
    if (size < MTDF_C_SUPER || size > fs->len - base)
        return mtdf_fail(fs, MTDF_ERR_FORMAT, "cramfs size %u outside the image", size);
    if ((root->mode & MTDF_S_IFMT) != MTDF_S_IFDIR)
        return mtdf_fail(fs, MTDF_ERR_FORMAT, "cramfs root is not a directory");
    c->base = base;
    c->size = base + size;
    return MTDF_OK;
}

static int mtdf_c_check_crc(mtdf_fs_t *fs, mtdf_cramfs_t *c)
{
    static const uint8_t zero[4] = {0, 0, 0, 0};
    const uint8_t *sb;
    uint32_t crc = 0xffffffffu;
    int status;

    if (!(c->flags & MTDF_C_FLAG_FSID_V2))
        return MTDF_OK;
    status = mtdf_src_crc(fs, c->base, MTDF_C_CRC_OFFSET, crc, &crc);
    if (status != MTDF_OK)
        return status;
    crc = mtdf_crc(fs, crc, zero, 4);
    status = mtdf_src_crc(fs, c->base + MTDF_C_CRC_OFFSET + 4, c->size - c->base - MTDF_C_CRC_OFFSET - 4, crc, &crc);
    if (status == MTDF_OK)
        status = mtdf_src_get(fs, c->base + MTDF_C_CRC_OFFSET, 4, NULL, &sb);
    if (status != MTDF_OK)
        return status;
    if ((crc ^ 0xffffffffu) != mtdf_get32(sb, c->big))
        fs->report.bad_image_crc++;
    return MTDF_OK;
}

static int mtdf_c_seen(mtdf_fs_t *fs, mtdf_cramfs_t *c, uint32_t offset, int *out_seen)
{
    uint32_t *slots;
    size_t mask;
    size_t i;
    int status;

    if (c->seen.len * 2 >= c->seen.cap) {
        mtdf_vec_t bigger = {NULL, 0, 0};
        size_t k;
        status = mtdf_vec_grow(fs, &bigger, sizeof(uint32_t), c->seen.cap ? c->seen.cap * 2 : 64);
        if (status != MTDF_OK)
            return status;
        memset(bigger.data, 0xff, bigger.cap * sizeof(uint32_t));
        for (k = 0; k < c->seen.cap; k++) {
            uint32_t value = ((uint32_t *)c->seen.data)[k];
            if (value == MTDF_NONE)
                continue;
            for (i = value * 2654435761u & (bigger.cap - 1); ((uint32_t *)bigger.data)[i] != MTDF_NONE;
                 i = (i + 1) & (bigger.cap - 1))
                ;
            ((uint32_t *)bigger.data)[i] = value;
        }
        bigger.len = c->seen.len;
        mtdf_vec_free(fs, &c->seen);
        c->seen = bigger;
    }
    slots = c->seen.data;
    mask = c->seen.cap - 1;
    for (i = offset * 2654435761u & mask; slots[i] != MTDF_NONE; i = (i + 1) & mask) {
        if (slots[i] == offset) {
            *out_seen = 1;
            return MTDF_OK;
        }
    }
    slots[i] = offset;
    c->seen.len++;
    *out_seen = 0;
    return MTDF_OK;
}

static int mtdf_c_push(mtdf_fs_t *fs, mtdf_cramfs_t *c, uint32_t entry, const mtdf_cinode_t *dir)
{
    mtdf_cwalk_t *top;
    int status;

    status = mtdf_vec_grow(fs, &c->stack, sizeof(mtdf_cwalk_t), 1);
    if (status != MTDF_OK)
        return status;
    top = (mtdf_cwalk_t *)c->stack.data + c->stack.len++;
    top->entry = entry;
    top->offset = dir->offset;
    top->size = dir->size;
    return MTDF_OK;
}

static void mtdf_c_attributes(mtdf_cramfs_t *c, const mtdf_cinode_t *inode, mtdf_entry_t *entry)
{
    uint32_t type = inode->mode & MTDF_S_IFMT;

    (void)c;
    entry->mode = inode->mode;
    entry->uid = inode->uid;
    entry->gid = inode->gid;
    entry->size = inode->size;
    entry->ref = inode->offset;
    if (type == MTDF_S_IFCHR || type == MTDF_S_IFBLK) {
        entry->dev_major = (inode->size >> 8) & 0xffu;
        entry->dev_minor = inode->size & 0xffu;
        entry->size = 0;
    } else if (type == MTDF_S_IFIFO || type == MTDF_S_IFSOCK) {
        entry->size = 0;
    }
}

static int mtdf_c_child(mtdf_fs_t *fs, mtdf_cramfs_t *c, const mtdf_cwalk_t *dir, uint64_t *pos, uint64_t end)
{
    const uint8_t *p;
    mtdf_cinode_t inode;
    uint32_t index;
    size_t name_len;
    int status;

    if (end - *pos < 12 || mtdf_src_get(fs, *pos, 12, NULL, &p) != MTDF_OK)
        return fs->report.bad_structure++, MTDF_ERR_CORRUPT;
    mtdf_c_inode(p, c->big, &inode);
    if (inode.namelen > end - *pos - 12 || mtdf_src_get(fs, *pos + 12, inode.namelen, NULL, &p) != MTDF_OK)
        return fs->report.bad_structure++, MTDF_ERR_CORRUPT;
    *pos += 12 + inode.namelen;
    fs->report.nodes++;
    name_len = mtdf_strnlen((const char *)p, inode.namelen);
    if (!mtdf_name_ok((const char *)p, name_len))
        return fs->report.bad_name++, MTDF_OK;
    status = mtdf_tree_add(fs, (const char *)p, name_len, &index);
    if (status != MTDF_OK)
        return status;
    mtdf_c_attributes(c, &inode, mtdf_entry(fs, index));
    mtdf_entry(fs, index)->parent = dir->entry;
    if ((inode.mode & MTDF_S_IFMT) == MTDF_S_IFDIR)
        return mtdf_c_push(fs, c, index, &inode);
    return MTDF_OK;
}

static int mtdf_c_directory(mtdf_fs_t *fs, mtdf_cramfs_t *c, mtdf_cwalk_t dir)
{
    uint64_t pos = dir.offset;
    uint64_t end = (uint64_t)dir.offset + dir.size;
    int seen;
    int status;

    if (dir.size == 0)
        return MTDF_OK;
    status = mtdf_c_seen(fs, c, dir.offset, &seen);
    if (status != MTDF_OK)
        return status;
    if (seen) {
        fs->report.loops++;
        return MTDF_OK;
    }
    if (end > c->size) {
        fs->report.bad_structure++;
        return MTDF_OK;
    }
    while (pos < end) {
        status = mtdf_c_child(fs, c, &dir, &pos, end);
        if (status == MTDF_ERR_CORRUPT)
            return MTDF_OK;
        if (status != MTDF_OK)
            return status;
    }
    return MTDF_OK;
}

static int mtdf_c_walk(mtdf_fs_t *fs, mtdf_cramfs_t *c, const mtdf_cinode_t *root)
{
    mtdf_cwalk_t dir;
    int status;

    mtdf_c_attributes(c, root, mtdf_entry(fs, MTDF_ROOT));
    status = mtdf_c_push(fs, c, MTDF_ROOT, root);
    while (status == MTDF_OK && c->stack.len > 0) {
        dir = ((mtdf_cwalk_t *)c->stack.data)[--c->stack.len];
        status = mtdf_c_directory(fs, c, dir);
    }
    return status;
}

static int mtdf_c_pointer(mtdf_fs_t *fs, mtdf_cramfs_t *c, uint64_t at, uint32_t *out)
{
    const uint8_t *p;
    int status;

    if (at + 4 > c->size)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "cramfs block pointer at %llu outside the image",
                         (unsigned long long)at);
    status = mtdf_src_get(fs, at, 4, NULL, &p);
    if (status == MTDF_OK)
        *out = mtdf_get32(p, c->big);
    return status;
}

static int mtdf_c_block(mtdf_fs_t *fs, mtdf_cramfs_t *c, const mtdf_entry_t *entry, uint64_t index,
                        uint64_t *out_start, uint64_t *out_end, int *out_raw)
{
    int extended = (c->flags & MTDF_C_FLAG_EXT_BLOCK_POINTERS) != 0;
    uint32_t mask = extended ? ~(MTDF_C_BLOCK_UNCOMPRESSED | MTDF_C_BLOCK_DIRECT) : 0xffffffffu;
    uint64_t pages = (entry->size + MTDF_C_PAGE - 1) / MTDF_C_PAGE;
    uint32_t pointer;
    uint32_t previous = 0;
    int status;

    status = mtdf_c_pointer(fs, c, entry->ref + index * 4, &pointer);
    if (status == MTDF_OK && index > 0)
        status = mtdf_c_pointer(fs, c, entry->ref + (index - 1) * 4, &previous);
    if (status != MTDF_OK)
        return status;
    if (extended && ((pointer | previous) & MTDF_C_BLOCK_DIRECT))
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "cramfs direct block pointers are not supported");
    *out_start = index == 0 ? entry->ref + pages * 4 : (previous & mask);
    *out_end = pointer & mask;
    *out_raw = extended && (pointer & MTDF_C_BLOCK_UNCOMPRESSED);
    if (*out_end < *out_start || *out_end > c->size)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "cramfs block %llu+%lld outside the image",
                         (unsigned long long)*out_start, (long long)(*out_end - *out_start));
    return MTDF_OK;
}

static int mtdf_c_page(mtdf_fs_t *fs, mtdf_cramfs_t *c, const mtdf_entry_t *entry, uint64_t index,
                       const uint8_t **out)
{
    uint64_t expected = entry->size - index * MTDF_C_PAGE < MTDF_C_PAGE ? entry->size - index * MTDF_C_PAGE
                                                                       : MTDF_C_PAGE;
    uint64_t key = (entry->ref << 20 | index) + 1;
    uint64_t start;
    uint64_t end;
    const uint8_t *block;
    int raw;
    int status;

    if (fs->unit_valid && fs->unit_key == key) {
        *out = fs->unit;
        return MTDF_OK;
    }
    status = mtdf_unit_buffer(fs);
    if (status == MTDF_OK)
        status = mtdf_c_block(fs, c, entry, index, &start, &end, &raw);
    if (status == MTDF_OK)
        status = mtdf_charge(fs, expected);
    fs->unit_valid = 0;
    if (status != MTDF_OK)
        return status;
    if (start == end) {
        memset(fs->unit, 0, (size_t)expected);
    } else if (raw) {
        if (end - start != expected)
            return mtdf_fail(fs, MTDF_ERR_CORRUPT, "cramfs uncompressed block has the wrong size");
        status = mtdf_src_get(fs, start, (size_t)expected, NULL, &block);
        if (status == MTDF_OK)
            memcpy(fs->unit, block, (size_t)expected);
    } else {
        if (end - start > MTDF_UNIT_MAX)
            return mtdf_fail(fs, MTDF_ERR_CORRUPT, "cramfs block of %llu bytes", (unsigned long long)(end - start));
        status = mtdf_src_get(fs, start, (size_t)(end - start), fs->unit + MTDF_UNIT_MAX, &block);
        if (status == MTDF_OK)
            status = mtdf_inflate(fs, block, (size_t)(end - start), fs->unit, (size_t)expected, 0);
    }
    if (status != MTDF_OK)
        return status;
    fs->unit_valid = 1;
    fs->unit_key = key;
    *out = fs->unit;
    return MTDF_OK;
}

static int mtdf_c_read(mtdf_fs_t *fs, const mtdf_entry_t *entry, uint64_t off, uint8_t *buf, size_t len)
{
    mtdf_cramfs_t *c = mtdf_c(fs);
    const uint8_t *page;
    uint64_t index;
    size_t inner;
    size_t piece;
    int status;

    while (len > 0) {
        index = off / MTDF_C_PAGE;
        inner = (size_t)(off % MTDF_C_PAGE);
        piece = MTDF_C_PAGE - inner < len ? MTDF_C_PAGE - inner : len;
        status = mtdf_c_page(fs, c, entry, index, &page);
        if (status != MTDF_OK)
            return status;
        memcpy(buf, page + inner, piece);
        buf += piece;
        off += piece;
        len -= piece;
    }
    return MTDF_OK;
}

static void mtdf_c_destroy(mtdf_fs_t *fs)
{
    mtdf_cramfs_t *c = mtdf_c(fs);

    mtdf_vec_free(fs, &c->seen);
    mtdf_vec_free(fs, &c->stack);
    mtdf_free(fs, c);
    fs->state = NULL;
}

static const mtdf_driver_t mtdf_cramfs_driver = {mtdf_c_read, mtdf_c_destroy};

int mtdf_cramfs_open(mtdf_fs_t *fs, int forced)
{
    mtdf_cramfs_t probe;
    mtdf_cramfs_t *c;
    mtdf_cinode_t root;
    int status;

    (void)forced;
    memset(&probe, 0, sizeof(probe));
    status = mtdf_c_superblock(fs, 0, &probe, &root);
    if (status == MTDF_ERR_FORMAT && fs->len > MTDF_C_SHIFTED)
        status = mtdf_c_superblock(fs, MTDF_C_SHIFTED, &probe, &root);
    if (status != MTDF_OK)
        return status;
    c = mtdf_alloc(fs, sizeof(*c));
    if (c == NULL)
        return MTDF_ERR_NOMEM;
    *c = probe;
    fs->state = c;
    fs->driver = &mtdf_cramfs_driver;
    fs->info.format = MTDF_FORMAT_CRAMFS;
    fs->info.big_endian = (uint32_t)c->big;
    fs->info.image_size = c->size;
    fs->info.unit_size = MTDF_C_PAGE;
    status = mtdf_c_check_crc(fs, c);
    if (status == MTDF_OK)
        status = mtdf_c_walk(fs, c, &root);
    mtdf_vec_free(fs, &c->seen);
    mtdf_vec_free(fs, &c->stack);
    return status;
}
