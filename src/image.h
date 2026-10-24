/* image.h - PNG output and image input (ImageIO) */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* rgba: w*h*4 bytes. flip_y: rows are bottom-up (as returned by glReadPixels). */
bool image_write_png(const char *path, int w, int h, const uint8_t *rgba, bool flip_y);
/* any image ImageIO reads, as w*h*4 RGBA bytes, top row first, premultiplied alpha; NULL on failure; free() it */
uint8_t *image_read(const char *path, int *w, int *h);
