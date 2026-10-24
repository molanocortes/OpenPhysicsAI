/* STL surface reused by 3D physics and native output; SI signed distances, no silent repair. */
#pragma once
#include "../geom/surface.h"
typedef struct LabMesh { Surface surface; Bvh bvh; } LabMesh;
bool labmesh_load(LabMesh *m, const char *path, double metres_per_unit, const char *sha256, char *err, size_t nerr);
void labmesh_free(LabMesh *m);
double labmesh_sdf(const LabMesh *m, const double p[3]);
