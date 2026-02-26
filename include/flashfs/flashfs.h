#ifndef FLASHFS_H
#define FLASHFS_H

#include <stddef.h>

#if defined(MTDF_STATIC) && defined(__GNUC__)
#define MTDF_API static __attribute__((unused))
#elif defined(MTDF_STATIC)
#define MTDF_API static
#elif defined(_WIN32) && defined(MTDF_BUILD_SHARED)
#define MTDF_API __declspec(dllexport)
#elif defined(_WIN32) && defined(MTDF_SHARED)
#define MTDF_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(MTDF_BUILD_SHARED)
#define MTDF_API __attribute__((visibility("default")))
#else
#define MTDF_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MTDF_OK 0
#define MTDF_ERR_ARG 1
#define MTDF_ERR_NOMEM 2

typedef struct mtdf_buf mtdf_buf_t;

typedef void *(*mtdf_alloc_fn)(void *ctx, size_t size);
typedef void (*mtdf_free_fn)(void *ctx, void *ptr);
typedef int (*mtdf_piece_fn)(void *ctx, const unsigned char *piece, size_t piece_len);

MTDF_API int mtdf_buf_create(mtdf_alloc_fn alloc_fn, mtdf_free_fn free_fn, void *alloc_ctx, mtdf_buf_t **out_buf);
MTDF_API void mtdf_buf_destroy(mtdf_buf_t *buf);
MTDF_API int mtdf_buf_append(mtdf_buf_t *buf, const unsigned char *data, size_t data_len);
MTDF_API size_t mtdf_buf_len(const mtdf_buf_t *buf);
MTDF_API const unsigned char *mtdf_buf_data(const mtdf_buf_t *buf);
MTDF_API int mtdf_buf_split(const mtdf_buf_t *buf, unsigned char sep, mtdf_piece_fn piece_fn, void *piece_ctx);
MTDF_API const char *mtdf_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
