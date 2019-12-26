#ifndef CG_RENDER_H
#define CG_RENDER_H

#include "output.h"

void output_render(struct cg_output *output, pixman_region32_t *damage);

#endif
