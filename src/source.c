#include "flashfs_int.h"

static int mtdf_src_fill(mtdf_fs_t *fs, uint64_t offset, size_t len, uint8_t *dst)
{
    if (fs->read_fn(fs->read_ctx, offset, dst, len) != 0) {
        fs->io_failed = 1;
        return mtdf_fail(fs, MTDF_ERR_IO, "read of %zu bytes at %llu failed", len, (unsigned long long)offset);
    }
    return MTDF_OK;
}

static int mtdf_src_window(mtdf_fs_t *fs, uint64_t offset, size_t len, const uint8_t **out)
{
    uint64_t left;
    int status;

    if (fs->window_len > 0 && offset >= fs->window_off && offset - fs->window_off <= fs->window_len &&
        len <= fs->window_len - (size_t)(offset - fs->window_off)) {
        *out = fs->window + (offset - fs->window_off);
        return MTDF_OK;
    }
    left = fs->len - offset;
    fs->window_len = 0;
    status = mtdf_src_fill(fs, offset, left < MTDF_WINDOW ? (size_t)left : MTDF_WINDOW, fs->window);
    if (status != MTDF_OK)
        return status;
    fs->window_off = offset;
    fs->window_len = left < MTDF_WINDOW ? (size_t)left : MTDF_WINDOW;
    *out = fs->window;
    return MTDF_OK;
}

int mtdf_src_get(mtdf_fs_t *fs, uint64_t offset, size_t len, uint8_t *scratch, const uint8_t **out)
{
    int status;

    if (offset > fs->len || len > fs->len - offset)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "span %llu+%zu outside the %llu-byte image",
                         (unsigned long long)offset, len, (unsigned long long)fs->len);
    if (fs->mem != NULL) {
        *out = fs->mem + offset;
        return MTDF_OK;
    }
    if (len == 0) {
        *out = fs->window;
        return MTDF_OK;
    }
    if (len <= MTDF_SMALL || scratch == NULL)
        return len <= MTDF_SMALL ? mtdf_src_window(fs, offset, len, out) : MTDF_ERR_ARG;
    status = mtdf_src_fill(fs, offset, len, scratch);
    *out = scratch;
    return status;
}

int mtdf_src_crc(mtdf_fs_t *fs, uint64_t offset, uint64_t len, uint32_t crc, uint32_t *out_crc)
{
    const uint8_t *data;
    size_t piece;
    int status;

    while (len > 0) {
        piece = len < MTDF_SMALL || fs->mem != NULL ? (size_t)(len < SIZE_MAX ? len : SIZE_MAX) : MTDF_SMALL;
        status = mtdf_src_get(fs, offset, piece, NULL, &data);
        if (status != MTDF_OK)
            return status;
        crc = mtdf_crc(fs, crc, data, piece);
        offset += piece;
        len -= piece;
    }
    *out_crc = crc;
    return MTDF_OK;
}

int mtdf_all_ff(mtdf_fs_t *fs, uint64_t offset, uint64_t len, int *out_erased)
{
    const uint8_t *data;
    size_t piece;
    size_t i;
    int status;

    *out_erased = 0;
    while (len > 0) {
        piece = len < MTDF_SMALL ? (size_t)len : MTDF_SMALL;
        status = mtdf_src_get(fs, offset, piece, NULL, &data);
        if (status != MTDF_OK)
            return status;
        for (i = 0; i < piece; i++) {
            if (data[i] != 0xff)
                return MTDF_OK;
        }
        offset += piece;
        len -= piece;
    }
    *out_erased = 1;
    return MTDF_OK;
}

void mtdf_crc_init(mtdf_fs_t *fs)
{
    uint32_t value;
    unsigned i;
    unsigned k;

    for (i = 0; i < 256; i++) {
        value = i;
        for (k = 0; k < 8; k++)
            value = value & 1 ? (value >> 1) ^ 0xedb88320u : value >> 1;
        fs->crc_table[0][i] = value;
    }
    for (i = 0; i < 256; i++) {
        value = fs->crc_table[0][i];
        for (k = 1; k < 8; k++) {
            value = fs->crc_table[0][value & 0xff] ^ (value >> 8);
            fs->crc_table[k][i] = value;
        }
    }
}

uint32_t mtdf_crc(const mtdf_fs_t *fs, uint32_t crc, const uint8_t *data, size_t len)
{
    const uint32_t (*t)[256] = fs->crc_table;
    uint32_t low;
    uint32_t high;

    while (len >= 8) {
        low = crc ^ mtdf_le32(data);
        high = mtdf_le32(data + 4);
        crc = t[7][low & 0xff] ^ t[6][(low >> 8) & 0xff] ^ t[5][(low >> 16) & 0xff] ^ t[4][low >> 24] ^
              t[3][high & 0xff] ^ t[2][(high >> 8) & 0xff] ^ t[1][(high >> 16) & 0xff] ^ t[0][high >> 24];
        data += 8;
        len -= 8;
    }
    while (len-- > 0)
        crc = t[0][(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return crc;
}
