/* Pixelica - private draw validation shared by the pipeline and the scene. */
#ifndef PIXELICA_DRAW_INTERNAL_H
#define PIXELICA_DRAW_INTERNAL_H

#include "pixelica/pixelica.h"

/* O(1) checks: mesh non-NULL, index_count % 3 == 0, non-NULL arrays for
 * non-zero counts, material texture/UV presence and texture dimensions.
 * PIXELICA_OK or PIXELICA_ERR_INVALID_ARG. */
PIXELICA_Status pixelica_check_mesh_structure(const PIXELICA_Mesh *mesh,
                                    const PIXELICA_Material *material);

/* O(n) checks (call after pixelica_check_mesh_structure): every index <
 * vertex_count and every supplied UV finite. PIXELICA_OK or
 * PIXELICA_ERR_INVALID_ARG. */
PIXELICA_Status pixelica_check_mesh_data(const PIXELICA_Mesh *mesh,
                               const PIXELICA_Material *material);

#endif /* PIXELICA_DRAW_INTERNAL_H */
