#include "labwater.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool labwater_volume(const LabFrame *frame,double spacing,const char *field,LabFrame *out){
    memset(out,0,sizeof *out);
    if(!isfinite(spacing) || spacing<=0)return false;
    const LabPart *p=NULL;
    for(int i=0;i<frame->nparts;i++)if(frame->parts[i].kind==LAB_POINTS){if(p)return false;p=frame->parts+i;}
    if(!p || p->npoints<=0)return false;
    const LabField *f=lab_find_field(p,field);if(!f || f->count!=(size_t)p->npoints)return false;
    double low[3]={INFINITY,INFINITY,INFINITY},high[3]={-INFINITY,-INFINITY,-INFINITY};
    for(int i=0;i<p->npoints;i++)for(int k=0;k<3;k++){
        double x=p->xyz[3*i+k];if(!isfinite(x))return false;
        low[k]=fmin(low[k],x);high[k]=fmax(high[k],x);
    }
    double h=1.3*spacing,step=spacing*.5;
    LabBlock b={0};size_t count=1;
    for(int k=0;k<3;k++){
        b.origin[k]=floor((low[k]-2*h)/step)*step;b.dx[k]=step;
        double cells=ceil((high[k]+2*h-b.origin[k])/step);
        if(cells<2 || cells>512)return false;b.n[k]=(int)cells;count*=b.n[k];
    }
    /* Bound temporary storage to 64 MB on the shared 8 GB laptop. */
    if(count>8u*1024u*1024u)return false;
    out->parts=calloc(1,sizeof(LabPart));if(!out->parts)goto fail;out->nparts=1;
    LabPart *v=out->parts;v->kind=LAB_BLOCKS;v->nblocks=1;v->blocks=malloc(sizeof b);
    v->fields=calloc(2,sizeof(LabField));if(!v->blocks || !v->fields)goto fail;v->nfields=2;*v->blocks=b;
    for(int i=0;i<2;i++){
        snprintf(v->fields[i].name,sizeof v->fields[i].name,"%s",i?"coverage":field);
        v->fields[i].location=LAB_AT_CELL;v->fields[i].count=count;
        v->fields[i].data=calloc(count,sizeof(float));if(!v->fields[i].data)goto fail;
    }
    float *scalar=v->fields[0].data,*coverage=v->fields[1].data;
    double weight0=21./(16.*3.141592653589793*h*h*h)*spacing*spacing*spacing;
    for(int n=0;n<p->npoints;n++){
        if(!isfinite(f->data[n]))goto fail;
        int begin[3],end[3];
        for(int a=0;a<3;a++){
            begin[a]=(int)fmax(0,ceil((p->xyz[3*n+a]-2*h-b.origin[a])/step-.5));
            end[a]=(int)fmin(b.n[a]-1,floor((p->xyz[3*n+a]+2*h-b.origin[a])/step-.5));
        }
        for(int k=begin[2];k<=end[2];k++)for(int j=begin[1];j<=end[1];j++)for(int i=begin[0];i<=end[0];i++){
            double x=b.origin[0]+(i+.5)*step-p->xyz[3*n],y=b.origin[1]+(j+.5)*step-p->xyz[3*n+1],z=b.origin[2]+(k+.5)*step-p->xyz[3*n+2];
            double r2=x*x+y*y+z*z;if(r2>=4*h*h)continue;double q=sqrt(r2)/h;
            double t=1-.5*q,w=weight0*t*t*t*t*(2*q+1);size_t at=((size_t)k*b.n[1]+j)*b.n[0]+i;
            coverage[at]+=(float)w;scalar[at]+=(float)(w*f->data[n]);
        }
    }
    for(size_t i=0;i<count;i++)if(coverage[i]>1e-12f)scalar[i]/=coverage[i];
    out->time=frame->time;return true;
fail:lab_frame_free(out);return false;
}
