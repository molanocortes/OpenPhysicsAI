/* colormap.h - scientific colour maps and user-defined gradients */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CMAP_TURBO = 0,
    CMAP_CFD,      /* classic CFD rainbow: blue-cyan-green-yellow-red */
    CMAP_JET,
    CMAP_VIRIDIS,
    CMAP_PLASMA,
    CMAP_INFERNO,
    CMAP_MAGMA,
    CMAP_COOLWARM, /* diverging, good for pressure */
    CMAP_HYDRO,    /* deep water blues to foam */
    CMAP_ICEFIRE,  /* diverging, dark centre */
    CMAP_GRAY,
    CMAP_CUSTOM,
    CMAP_COUNT
} ColormapId;

const char *colormap_name(int id);
int colormap_find(const char *name); /* -1 if unknown */
void colormap_sample(int id, float t, float rgb[3]);
void colormap_fill_rgba(int id, uint8_t *rgba, int n);

/* Custom gradient: "#001030 #00ffcc #ff4000" (evenly spaced) or "0:#001030 0.7:#00ffcc 1:#ff4000". */
bool colormap_set_custom(const char *spec, char *err, size_t errlen);
