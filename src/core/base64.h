/* base64.h - RFC 4648 base64 (standard alphabet, padded) for images in control responses */
#pragma once

#include <stddef.h>

/* malloc'd, NUL-terminated; NULL on allocation failure */
char *base64_encode(const unsigned char *data, size_t n, size_t *out_len);
/* strict decoder (no whitespace); NULL if the input is not valid base64 */
unsigned char *base64_decode(const char *s, size_t n, size_t *out_len);
