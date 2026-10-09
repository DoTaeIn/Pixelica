/*
 * Pixelica - result codes shared by every public module.
 *
 * Kept in its own header so math.h, renderer.h and pixelica.h can all use
 * PIXELICA_Status without include cycles. New codes are only ever appended.
 */
#ifndef PIXELICA_STATUS_H
#define PIXELICA_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PIXELICA_Status {
    PIXELICA_OK = 0,
    PIXELICA_ERR_INVALID_ARG, /* NULL/non-finite/out-of-range input or wrong call order */
    PIXELICA_ERR_NO_MEMORY,   /* allocation or caller-supplied memory block too small */
    PIXELICA_ERR_CAPACITY,    /* configured per-frame/per-mesh/asset limit exceeded */
    PIXELICA_ERR_BAD_ASSET,   /* malformed or corrupt asset data (see assets.h) */
    PIXELICA_ERR_PLATFORM,    /* a PAL callback reported failure */
    PIXELICA_ERR_NOT_FOUND    /* lookup (e.g. cart resource id) found nothing */
} PIXELICA_Status;

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_STATUS_H */
