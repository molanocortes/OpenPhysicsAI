/* impact_materials.h - the material records impact runs may name (impact_materials.c holds each value's source). */
#pragma once

#include "impact.h"

const ImMaterial *im_material_find(const char *name); /* NULL if unknown */
int im_material_count(void);
const ImMaterial *im_material_at(int i);
