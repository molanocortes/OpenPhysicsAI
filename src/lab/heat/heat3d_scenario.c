/* Explicit 3D conduction scenarios. No fluid transport is implied. */
#include "heat3d.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "../../core/json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool lab_run_heat3d(const JsonValue *root,const char *out,bool quiet,LabRunInfo *info,char *err,size_t el) {
    const JsonValue *grid=json_get(root,"grid"),*mat=json_get(root,"material"),*run=json_get(root,"run");
    int n[3]={(int)json_get_int(grid,"nx",0),(int)json_get_int(grid,"ny",0),(int)json_get_int(grid,"nz",0)};
    double dx=json_get_num(grid,"cell_m",0),k=json_get_num(mat,"conductivity_w_mk",0),
        cp=json_get_num(mat,"volumetric_heat_capacity_j_m3k",0),T=json_get_num(root,"initial_temperature_k",NAN),
        end=json_get_num(run,"end_time_s",0);
    int frames=(int)json_get_int(run,"frames",0);
    if(!json_get_str(mat,"source",NULL)||!(k>0)||!(cp>0)||!isfinite(k)||!isfinite(cp)||!isfinite(T)||
       !(end>0)||!isfinite(end)||frames<1||frames>10000||
       strcmp(json_get_str(root,"boundary",""),"insulated")) {
        snprintf(err,el,"3D conduction requires sourced positive material properties, initial temperature, explicit insulated boundary, positive end time and 1..10000 frames");return false;
    }
    Heat3D h;if(!heat3d_create(&h,n,dx)){snprintf(err,el,"invalid or oversized 3D heat grid (2..512 per axis, at most 8 million cells)");return false;}
    for(size_t q=0;q<h.count;q++){h.T[q]=T;h.capacity[q]=cp;h.conductivity[q]=k;}
    const JsonValue *regions=json_get(root,"regions");
    for(size_t r=0;r<json_len(regions);r++) {
        const JsonValue *region=json_at(regions,r);double box[6];
        double rk=json_get_num(region,"conductivity_w_mk",k),rc=json_get_num(region,"volumetric_heat_capacity_j_m3k",cp),
            source=json_get_num(region,"heat_w_m3",0),initial=json_get_num(region,"initial_temperature_k",T);
        if(!json_get_numbers(json_get(region,"box_m"),box,6)||!json_get_str(region,"source",NULL)||
           !isfinite(rk)||rk<=0||!isfinite(rc)||rc<=0||!isfinite(source)||!isfinite(initial)) {
            snprintf(err,el,"regions[%zu]: require box_m, source and finite positive material properties",r);heat3d_free(&h);return false;
        }
        for(int d=0;d<3;d++)if(!isfinite(box[d])||!isfinite(box[d+3])||box[d]<0||box[d+3]<=box[d]||box[d+3]>n[d]*dx){
            snprintf(err,el,"regions[%zu]: box must lie inside grid",r);heat3d_free(&h);return false;
        }
        size_t q=0;
        for(int z=0;z<n[2];z++)for(int y=0;y<n[1];y++)for(int x=0;x<n[0];x++,q++) {
            double pos[3]={(x+.5)*dx,(y+.5)*dx,(z+.5)*dx};bool inside=true;
            for(int d=0;d<3;d++)if(pos[d]<box[d]||pos[d]>=box[d+3])inside=false;
            if(inside){h.conductivity[q]=rk;h.capacity[q]=rc;h.source[q]=source;h.T[q]=initial;}
        }
    }
    /* Serialize metadata through JSON so arbitrary titles remain valid. */
    JsonValue *meta=json_object();
    bool metadata_ok=meta && json_set_string(meta,"domain","heat") &&
        json_set_string(meta,"title",json_get_str(root,"title","3D conduction")) &&
        json_set_string(meta,"model","3D conduction, no fluid flow") &&
        json_set_string(meta,"volume_look","solid") && /* the volume is a body: seen from outside, cut to look in */
        json_set(meta,"scenario",json_clone(root));
    JsonValue *fields=json_set_object(meta,"fields");
    metadata_ok=metadata_ok && fields && json_set_string(fields,"T","K");
    char *header=metadata_ok?json_dump(meta,0,NULL,NULL):NULL;json_free(meta);
    if(!header){snprintf(err,el,"cannot allocate result metadata");heat3d_free(&h);return false;}
    LabWriter *w=lab_create(out,header,err,el);free(header);if(!w){heat3d_free(&h);return false;}
    bool ok=true;long steps=0;
    LabBlock b={{n[0],n[1],n[2]},0,LAB_PLANE_XY,{0,0,0},{dx,dx,dx}};
    for(int f=0;f<=frames&&ok;f++) {
        double target=end*f/frames;
        while(h.time<target-1e-12*fmax(1,target)) {
            if(!heat3d_step(&h,target-h.time)){snprintf(err,el,"invalid 3D thermal state at %.9g s",h.time);ok=false;break;}steps++;
        }
        if(!ok)break;
        lab_frame_begin(w,h.time);lab_part_blocks(w,"temperature_volume",1,&b);lab_field_d(w,"T",LAB_AT_CELL,h.count,h.T);
        ok=lab_frame_end(w);
        if(!quiet)fprintf(stderr,"  3D heat frame %d/%d, %.6g s, %ld steps\n",f,frames,h.time,steps);
    }
    info->frames=lab_frames_written(w);info->steps=steps;
    snprintf(info->diagnostics,sizeof info->diagnostics,"{\"energy_j\":%.12g,\"end_time_s\":%.9g}",heat3d_energy(&h),h.time);
    if(!lab_close(w))ok=false;heat3d_free(&h);return ok;
}
