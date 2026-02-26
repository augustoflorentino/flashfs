#define MTDF_IMPLEMENTATION
#include "flashfs/flashfs.h"

static int count_piece(void *ctx, const unsigned char *piece, size_t piece_len)
{
    (void)piece;
    (void)piece_len;
    (*(int *)ctx)++;
    return 0;
}

int main(void)
{
    mtdf_buf_t *buf = NULL;
    int pieces = 0;
    int status;

    status = mtdf_buf_create(NULL, NULL, NULL, &buf);
    if (status == MTDF_OK)
        status = mtdf_buf_append(buf, (const unsigned char *)"a\nb", 3);
    if (status == MTDF_OK)
        status = mtdf_buf_split(buf, '\n', count_piece, &pieces);
    if (mtdf_buf_len(buf) != 3 || mtdf_buf_data(buf) == NULL || mtdf_strerror(status) == NULL)
        status = MTDF_ERR_ARG;
    mtdf_buf_destroy(buf);
    return status == MTDF_OK && pieces == 2 ? 0 : 1;
}
