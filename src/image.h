/* image.h - PNG output (ImageIO) */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* rgba: w*h*4 bytes. flip_y: rows are bottom-up (as returned by glReadPixels). */
bool image_write_png(const char *path, int w, int h, const uint8_t *rgba, bool flip_y);
