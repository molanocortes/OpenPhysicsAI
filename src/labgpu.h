/* Retained OpenGL renderer for computed 3D lab geometry. Owned by the native app context. */
#pragma once
#include "lab/labscene.h"
typedef struct LabGpu LabGpu;
LabGpu *labgpu_create(void);
void labgpu_destroy(LabGpu *g);
bool labgpu_upload(LabGpu *g,const LabScene *s);
bool labgpu_draw(LabGpu *g,const LabViewOpts *o,double lo,double hi,unsigned target,int x,int y,int w,int h);

void labgpu_refit(LabGpu *g);
/* the next frame has no computed volume (and no water surface) */
void labgpu_clear_volume(LabGpu *g);

bool labgpu_upload_volume(LabGpu *g,const LabPart *p,const char *field);
/* the same, with the surfaces where iso_field crosses level drawn as solid surfaces (vortex cores by Q) */
bool labgpu_upload_volume_iso(LabGpu *g,const LabPart *p,const char *field,const char *iso_field,double level);
void labgpu_section(LabGpu *g,int axis,double fraction,bool flip);

void labgpu_water_surface(LabGpu *g,bool enabled);
