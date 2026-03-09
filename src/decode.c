#include "flashfs_int.h"

#ifdef MTDF_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef MTDF_HAVE_LZMA
#include <lzma.h>
#endif
#ifdef MTDF_HAVE_LZO
#include <lzo/lzo1x.h>
#endif

#if !defined(MTDF_HAVE_ZLIB) || !defined(MTDF_HAVE_LZMA) || !defined(MTDF_HAVE_LZO)
static int mtdf_missing(mtdf_fs_t *fs, const char *library, const char *what)
{
    return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "%s needs %s, which this build does not include", what, library);
}
#endif

#ifdef MTDF_HAVE_ZLIB
static voidpf mtdf_zalloc(voidpf opaque, uInt items, uInt size)
{
    if (size != 0 && items > SIZE_MAX / size)
        return Z_NULL;
    return mtdf_alloc((mtdf_fs_t *)opaque, (size_t)items * size);
}

static void mtdf_zfree(voidpf opaque, voidpf ptr)
{
    mtdf_free((mtdf_fs_t *)opaque, ptr);
}

static int mtdf_zlib_stream(mtdf_fs_t *fs, int window_bits, z_stream **out)
{
    z_stream *strm = fs->zlib;

    if (strm == NULL) {
        strm = mtdf_alloc(fs, sizeof(*strm));
        if (strm == NULL)
            return MTDF_ERR_NOMEM;
        memset(strm, 0, sizeof(*strm));
        strm->zalloc = mtdf_zalloc;
        strm->zfree = mtdf_zfree;
        strm->opaque = fs;
        if (inflateInit2(strm, window_bits) != Z_OK) {
            mtdf_free(fs, strm);
            return MTDF_ERR_NOMEM;
        }
        fs->zlib = strm;
    } else if (inflateReset2(strm, window_bits) != Z_OK) {
        return MTDF_ERR_NOMEM;
    }
    *out = strm;
    return MTDF_OK;
}
#endif

int mtdf_inflate(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, int raw)
{
#ifdef MTDF_HAVE_ZLIB
    z_stream *strm;
    int status;
    int z;

    if (in_len > UINT32_MAX || out_len > UINT32_MAX)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "zlib block too large");
    status = mtdf_zlib_stream(fs, raw ? -15 : 15, &strm);
    if (status != MTDF_OK)
        return status;
    strm->next_in = (Bytef *)(uintptr_t)in;
    strm->avail_in = (uInt)in_len;
    strm->next_out = out;
    strm->avail_out = (uInt)out_len;
    z = inflate(strm, Z_FINISH);
    if (z == Z_MEM_ERROR)
        return MTDF_ERR_NOMEM;
    if (z == Z_DATA_ERROR || z == Z_NEED_DICT || z == Z_STREAM_ERROR)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "zlib: %s", strm->msg != NULL ? strm->msg : "bad stream");
    if (strm->avail_out != 0)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "zlib block decodes to %zu bytes, expected %zu",
                         out_len - strm->avail_out, out_len);
    return MTDF_OK;
#else
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    (void)raw;
    return mtdf_missing(fs, "zlib", "zlib compression");
#endif
}

#ifdef MTDF_HAVE_LZMA
static void *mtdf_lzma_alloc(void *opaque, size_t nmemb, size_t size)
{
    if (size != 0 && nmemb > SIZE_MAX / size)
        return NULL;
    return mtdf_alloc((mtdf_fs_t *)opaque, nmemb * size);
}

static void mtdf_lzma_free(void *opaque, void *ptr)
{
    mtdf_free((mtdf_fs_t *)opaque, ptr);
}

typedef struct mtdf_lzma {
    lzma_stream strm;
    lzma_allocator allocator;
} mtdf_lzma_t;
#endif

int mtdf_unlzma_jffs2(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
#ifdef MTDF_HAVE_LZMA
    mtdf_lzma_t *state = fs->lzma;
    lzma_options_lzma options;
    lzma_filter filters[2];
    lzma_ret ret;

    if (state == NULL) {
        state = mtdf_alloc(fs, sizeof(*state));
        if (state == NULL)
            return MTDF_ERR_NOMEM;
        memset(state, 0, sizeof(*state));
        state->allocator.alloc = mtdf_lzma_alloc;
        state->allocator.free = mtdf_lzma_free;
        state->allocator.opaque = fs;
        state->strm.allocator = &state->allocator;
        fs->lzma = state;
    }
    memset(&options, 0, sizeof(options));
    options.dict_size = 0x2000;
    options.lc = 0;
    options.lp = 0;
    options.pb = 0;
    filters[0].id = LZMA_FILTER_LZMA1;
    filters[0].options = &options;
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = NULL;
    ret = lzma_raw_decoder(&state->strm, filters);
    if (ret != LZMA_OK)
        return ret == LZMA_MEM_ERROR ? MTDF_ERR_NOMEM : mtdf_fail(fs, MTDF_ERR_CORRUPT, "lzma setup failed");
    state->strm.next_in = in;
    state->strm.avail_in = in_len;
    state->strm.next_out = out;
    state->strm.avail_out = out_len;
    while (state->strm.avail_out > 0) {
        ret = lzma_code(&state->strm, LZMA_RUN);
        if (ret == LZMA_MEM_ERROR)
            return MTDF_ERR_NOMEM;
        if (ret != LZMA_OK)
            break;
        if (state->strm.avail_in == 0)
            break;
    }
    if (state->strm.avail_out != 0)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "lzma node decodes to %zu bytes, expected %zu",
                         out_len - state->strm.avail_out, out_len);
    return MTDF_OK;
#else
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    return mtdf_missing(fs, "liblzma", "jffs2 lzma compression");
#endif
}

int mtdf_unlzo(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
#ifdef MTDF_HAVE_LZO
    lzo_uint produced = out_len;

    if (lzo_init() != LZO_E_OK)
        return mtdf_fail(fs, MTDF_ERR_UNSUPPORTED, "liblzo2 failed to initialize");
    if (lzo1x_decompress_safe(in, in_len, out, &produced, NULL) != LZO_E_OK || produced != out_len)
        return mtdf_fail(fs, MTDF_ERR_CORRUPT, "lzo block does not decode to %zu bytes", out_len);
    return MTDF_OK;
#else
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    return mtdf_missing(fs, "liblzo2", "lzo compression");
#endif
}

int mtdf_unrtime(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    size_t positions[256];
    size_t produced = 0;
    size_t cursor = 0;
    size_t back;
    unsigned value;
    unsigned repeat;

    memset(positions, 0, sizeof(positions));
    while (produced < out_len) {
        if (in_len - cursor < 2)
            return mtdf_fail(fs, MTDF_ERR_CORRUPT, "rtime input ends early");
        value = in[cursor];
        repeat = in[cursor + 1];
        cursor += 2;
        out[produced++] = (uint8_t)value;
        back = positions[value];
        positions[value] = produced;
        if (repeat > out_len - produced)
            return mtdf_fail(fs, MTDF_ERR_CORRUPT, "rtime run past the end");
        while (repeat-- > 0)
            out[produced++] = out[back++];
    }
    return MTDF_OK;
}

void mtdf_decoders_free(mtdf_fs_t *fs)
{
#ifdef MTDF_HAVE_ZLIB
    if (fs->zlib != NULL) {
        inflateEnd(fs->zlib);
        mtdf_free(fs, fs->zlib);
    }
#endif
#ifdef MTDF_HAVE_LZMA
    if (fs->lzma != NULL) {
        lzma_end(&((mtdf_lzma_t *)fs->lzma)->strm);
        mtdf_free(fs, fs->lzma);
    }
#endif
    fs->zlib = NULL;
    fs->lzma = NULL;
}
