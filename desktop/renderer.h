#ifndef CG_RENDERER_H
#define CG_RENDERER_H

#include "output.h"

void cage_renderer_render_output(struct cg_output *output, pixman_region32_t *damage);

#endif
