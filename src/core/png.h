/* png.h - dependency-free PNG encoder (8-bit RGB/RGBA, zlib deflate with LZ77 and fixed Huffman codes) */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* channels: 3 (RGB) or 4 (RGBA); rows top to bottom. *out is malloc'd. */
bool png_encode(const unsigned char *pixels, int width, int height, int channels, unsigned char **out, size_t *out_len);
bool png_write_file(const char *path, const unsigned char *pixels, int width, int height, int channels);
/* zlib stream (RFC 1950) of arbitrary data, exposed for tests and VTU compression */
bool zlib_compress(const unsigned char *data, size_t n, unsigned char **out, size_t *out_len);
unsigned long crc32_update(unsigned long crc, const unsigned char *data, size_t n);
