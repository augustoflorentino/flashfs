#include "flashfs/flashfs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct mtdf_buf {
    mtdf_alloc_fn alloc_fn;
    mtdf_free_fn free_fn;
    void *alloc_ctx;
    unsigned char *data;
    size_t len;
    size_t cap;
};

static void *mtdf_default_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void mtdf_default_free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

int mtdf_buf_create(mtdf_alloc_fn alloc_fn, mtdf_free_fn free_fn, void *alloc_ctx, mtdf_buf_t **out_buf)
{
    mtdf_buf_t *buf;

    if (out_buf == NULL || (alloc_fn == NULL) != (free_fn == NULL))
        return MTDF_ERR_ARG;
    *out_buf = NULL;
    if (alloc_fn == NULL) {
        alloc_fn = mtdf_default_alloc;
        free_fn = mtdf_default_free;
    }
    buf = alloc_fn(alloc_ctx, sizeof(*buf));
    if (buf == NULL)
        return MTDF_ERR_NOMEM;
    memset(buf, 0, sizeof(*buf));
    buf->alloc_fn = alloc_fn;
    buf->free_fn = free_fn;
    buf->alloc_ctx = alloc_ctx;
    *out_buf = buf;
    return MTDF_OK;
}

void mtdf_buf_destroy(mtdf_buf_t *buf)
{
    if (buf == NULL)
        return;
    if (buf->data != NULL)
        buf->free_fn(buf->alloc_ctx, buf->data);
    buf->free_fn(buf->alloc_ctx, buf);
}

static int mtdf_buf_reserve(mtdf_buf_t *buf, size_t needed)
{
    size_t cap;
    unsigned char *data;

    if (needed <= buf->cap)
        return MTDF_OK;
    cap = buf->cap ? buf->cap : 64;
    while (cap < needed)
        cap = cap > SIZE_MAX / 2 ? needed : cap * 2;
    data = buf->alloc_fn(buf->alloc_ctx, cap);
    if (data == NULL)
        return MTDF_ERR_NOMEM;
    if (buf->len > 0)
        memcpy(data, buf->data, buf->len);
    if (buf->data != NULL)
        buf->free_fn(buf->alloc_ctx, buf->data);
    buf->data = data;
    buf->cap = cap;
    return MTDF_OK;
}

int mtdf_buf_append(mtdf_buf_t *buf, const unsigned char *data, size_t data_len)
{
    int status;

    if (buf == NULL || (data == NULL && data_len > 0))
        return MTDF_ERR_ARG;
    if (data_len == 0)
        return MTDF_OK;
    if (data_len > SIZE_MAX - buf->len)
        return MTDF_ERR_NOMEM;
    status = mtdf_buf_reserve(buf, buf->len + data_len);
    if (status != MTDF_OK)
        return status;
    memcpy(buf->data + buf->len, data, data_len);
    buf->len += data_len;
    return MTDF_OK;
}

size_t mtdf_buf_len(const mtdf_buf_t *buf)
{
    return buf ? buf->len : 0;
}

const unsigned char *mtdf_buf_data(const mtdf_buf_t *buf)
{
    return buf ? buf->data : NULL;
}

int mtdf_buf_split(const mtdf_buf_t *buf, unsigned char sep, mtdf_piece_fn piece_fn, void *piece_ctx)
{
    size_t start = 0;
    size_t i;

    if (buf == NULL || piece_fn == NULL)
        return MTDF_ERR_ARG;
    for (i = 0; i < buf->len; i++) {
        if (buf->data[i] != sep)
            continue;
        if (piece_fn(piece_ctx, buf->data + start, i - start) != 0)
            return MTDF_OK;
        start = i + 1;
    }
    if (start < buf->len)
        piece_fn(piece_ctx, buf->data + start, buf->len - start);
    return MTDF_OK;
}

const char *mtdf_strerror(int status)
{
    switch (status) {
    case MTDF_OK:
        return "ok";
    case MTDF_ERR_ARG:
        return "invalid argument";
    case MTDF_ERR_NOMEM:
        return "out of memory";
    default:
        return "unknown status";
    }
}
