#include "labmesh.h"
#include "../core/sha256.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void labmesh_free(LabMesh *m) { bvh_free(&m->bvh); surface_free(&m->surface); memset(m,0,sizeof *m); }
bool labmesh_load(LabMesh *m,const char *path,double scale,const char *hash,char *err,size_t ne) {
    memset(m,0,sizeof *m);
    char actual[65];
    if (!path || !isfinite(scale) || !(scale>0) || !hash || strlen(hash)!=64 || !sha256_file(path,actual,NULL) || strcmp(hash,actual)) {
        snprintf(err,ne,"STL path, explicit positive unit scale and matching SHA-256 required"); return false;
    }
    Mesh raw; mesh_init(&raw);
    if (!mesh_load_stl_ex(path,&raw,NULL,8u<<20,50000,err,ne)) return false;
    SurfaceRepairOptions opt; surface_repair_defaults(&opt);
    opt.remove_degenerate=false; opt.remove_duplicate_faces=false; opt.orient_outward=false;
    SurfaceDiagnostics diag; SurfaceComponent *components=NULL;
    bool ok=surface_build(&raw,&opt,&m->surface,&components,&diag,err,ne);
    mesh_free(&raw); free(components);
    if (!ok) { labmesh_free(m); return false; }
    if (!diag.closed_solid || diag.degenerate || diag.duplicate_faces || diag.opposite_duplicates) {
        snprintf(err,ne,"STL must be closed, consistently oriented and nondegenerate; no silent repair"); labmesh_free(m); return false;
    }
    for(int i=0;i<3*m->surface.nv;i++)m->surface.v[i]*=scale;
    if (!bvh_build(&m->bvh,m->surface.v,m->surface.tri,m->surface.nt)) {
        snprintf(err,ne,"STL BVH allocation failed"); labmesh_free(m); return false;
    }
    if(surface_check_self_intersections(&m->surface,&m->bvh,1,&diag)>0) {
        snprintf(err,ne,"STL has self intersections"); labmesh_free(m); return false;
    }
    return true;
}
double labmesh_sdf(const LabMesh *m,const double p[3]) {
    double q[3],distance; int tri;
    if(!bvh_closest(&m->bvh,p,1e100,&tri,q,&distance))return 1e100;
    return surface_point_inside(&m->surface,&m->bvh,p)?-distance:distance;
}
