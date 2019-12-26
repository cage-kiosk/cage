/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#include <wlr/types/wlr_box.h>

#include "util.h"

int
scale_length(int length, int offset, float scale)
{
	/**
	 * One does not simply multiply the width by the scale. We allow fractional
	 * scaling, which means the resulting scaled width might be a decimal.
	 * So we round it.
	 *
	 * But even this can produce undesirable results depending on the X or Y
	 * offset of the box. For example, with a scale of 1.5, a box with
	 * width=1 should not scale to 2px if its X coordinate is 1, because the
	 * X coordinate would have scaled to 2px.
	 */
	return round((offset + length) * scale) - round(offset * scale);
}

void
scale_box(struct wlr_box *box, float scale)
{
	box->width = scale_length(box->width, box->x, scale);
	box->height = scale_length(box->height, box->y, scale);
	box->x = round(box->x * scale);
	box->y = round(box->y * scale);
}
