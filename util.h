#ifndef CG_UTIL_H
#define CG_UTIL_H

#include <wlr/types/wlr_box.h>

/** Apply scale to a width or height. */
int scale_length(int length, int offset, float scale);

void scale_box(struct wlr_box *box, float scale);

#endif
