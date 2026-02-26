#include "flashfs/flashfs.h"

#include <stdint.h>

static int consume_piece(void *ctx, const unsigned char *piece, size_t piece_len)
{
    size_t *total = ctx;

    *total += piece_len + (piece_len > 0 ? piece[0] : 0);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    mtdf_buf_t *buf = NULL;
    size_t total = 0;

    if (mtdf_buf_create(NULL, NULL, NULL, &buf) != MTDF_OK)
        return 0;
    if (mtdf_buf_append(buf, data, size) == MTDF_OK && size > 0)
        mtdf_buf_split(buf, data[0], consume_piece, &total);
    mtdf_buf_destroy(buf);
    return 0;
}
