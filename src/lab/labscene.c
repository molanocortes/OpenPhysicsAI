#include "labscene.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct Face { int key[4], v[4], cell; } Face;
static int compare(const void *a, const void *b) {
    const Face *x=a,*y=b;
    for(int k=0;k<4;k++) if(x->key[k]!=y->key[k]) return x->key[k]<y->key[k]?-1:1;
    return 0;
}
static void sort(int *v) { for(int i=1;i<4;i++) for(int j=i;j>0 && v[j]<v[j-1];j--) {int a=v[j];v[j]=v[j-1];v[j-1]=a;} }
void labscene_free(LabScene *s) { free(s->tri);free(s->points);free(s->lines);memset(s,0,sizeof *s); }
static bool volume_part(const LabPart *p) {
    if (p->kind != LAB_BLOCKS || p->nblocks != 1 || !p->nfields) return false;
    const LabBlock *b = p->blocks;
    size_t count = 1;
    for (int k = 0; k < 3; k++) {
        if (b->n[k] < 2 || b->n[k] > 2048 || !isfinite(b->origin[k]) || !isfinite(b->dx[k]) || b->dx[k] <= 0) return false;
        count *= (size_t)b->n[k];
    }
    for (int i = 0; i < p->nfields; i++)
        if (p->fields[i].count != count) return false;
    return true;
}

/* every part is geometry the GPU draws (points, surfaces, lines) or one computed 3D volume; planar blocks are not */
bool labscene_supported(const LabFrame *fr) {
    if(!fr || !fr->nparts) return false;
    int volumes=0;
    for(int p=0;p<fr->nparts;p++) {
        const LabPart *t=fr->parts+p;
        if(t->kind==LAB_BLOCKS && volume_part(t)) { volumes++; continue; }
        if(t->kind!=LAB_POINTS && t->kind!=LAB_CELLS) return false;
        if(t->kind==LAB_CELLS && t->cell_type!=LAB_HEX && t->cell_type!=LAB_TRI && t->cell_type!=LAB_QUAD && t->cell_type!=LAB_LINE) return false;
    }
    return volumes <= 1;
}
int labscene_look_id(const char *name) {
    static const char *N[LOOK_COUNT]={"","copper","magnet_north","magnet_south","steel","aluminium","glass","fabric","tissue"};
    for(int i=1;i<LOOK_COUNT;i++)if(name && !strcmp(name,N[i]))return i;
    return LOOK_NONE;
}
static int part_look(const LabPart *p,const LabViewOpts *o) {
    for(int i=0;i<o->nlooks;i++)if(!strcmp(o->looks[i].part,p->name))return o->looks[i].look;
    return LOOK_NONE;
}
static bool keep(double v,int axis,double cut,bool flip) {return axis<0 || (flip?v>=cut:v<=cut);}
static LabVertex vertex(const LabPart *p,const LabField *f,int i,int cell,const LabViewOpts *o) {
    LabVertex v={{(float)p->xyz[3*i],(float)p->xyz[3*i+1],(float)p->xyz[3*i+2]},0,0,0,{0,0,0}};
    size_t q=f && f->location==LAB_AT_CELL?(size_t)cell:(size_t)i;
    if(f && q<f->count && isfinite(f->data[q])) {
        double a=f->data[q];if(o->absval)a=fabs(a);if(o->logscale)a=log10(fmax(fabs(a),1e-30));
        v.value=(float)a;v.field=1;
    }
    int look=part_look(p,o);
    if(look)v.field=-(float)(1+look),v.value=0; /* a material look: labgpu.c draws the material, not the field */
    const LabField *r=o->true_size?lab_find_field(p,"radius"):NULL;
    v.radius=(float)(r && (size_t)i<r->count?r->data[i]:o->point_radius_m);
    return v;
}
/* Geometric buffer allocation is bounded by the result size and checked before writes. */
/* smooth normals for the triangles tri[a..b): each vertex takes the area-weighted normals of every triangle that shares
 * its position (welded on a grid of 1e-7 of the scene's size), so a meshed body shades smoothly */
static void smooth_normals(LabVertex *tri,size_t a,size_t b,double span) {
    size_t nv=b-a,cap=1;if(nv<3)return;
    while(cap<2*nv)cap<<=1;
    long long (*key)[3]=malloc(cap*sizeof *key);float (*acc)[3]=calloc(cap,sizeof *acc);unsigned char *used=calloc(cap,1);size_t *slot=malloc(nv*sizeof *slot);
    if(!key||!acc||!used||!slot){free(key);free(acc);free(used);free(slot);return;}
    double qz=span>0?span*1e-7:1e-9;
    for(size_t i=0;i<nv;i++) {
        long long k[3];for(int c=0;c<3;c++)k[c]=llround(tri[a+i].p[c]/qz);
        unsigned long long h=(unsigned long long)(k[0]*73856093LL^k[1]*19349663LL^k[2]*83492791LL);
        size_t j=(size_t)(h&(cap-1));
        while(used[j]&&(key[j][0]!=k[0]||key[j][1]!=k[1]||key[j][2]!=k[2]))j=(j+1)&(cap-1);
        if(!used[j]){used[j]=1;memcpy(key[j],k,sizeof k);}
        slot[i]=j;
    }
    for(size_t t=0;t+2<nv;t+=3) {
        const float *A=tri[a+t].p,*B=tri[a+t+1].p,*C=tri[a+t+2].p;
        float e1[3]={B[0]-A[0],B[1]-A[1],B[2]-A[2]},e2[3]={C[0]-A[0],C[1]-A[1],C[2]-A[2]};
        float fn[3]={e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]};
        /* the triangles are taken as oriented consistently (labshape.c makes them so; STL files are) */
        for(int q=0;q<3;q++)for(int c=0;c<3;c++)acc[slot[t+q]][c]+=fn[c];
    }
    for(size_t i=0;i<nv;i++) {
        float *n=acc[slot[i]];double l=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        for(int c=0;c<3;c++)tri[a+i].n[c]=l>0?(float)(n[c]/l):0;
    }
    free(key);free(acc);free(used);free(slot);
}
static bool reserve(LabVertex **v,size_t *capacity,size_t n) {
    if(n>SIZE_MAX/sizeof **v || n>INT_MAX) return false;
    if(n<=*capacity)return true;
    size_t c=n+n/2+64;if(c>INT_MAX)c=n;
    void *q=realloc(*v,c*sizeof **v);if(!q)return false;*v=q;*capacity=c;return true;
}
bool labscene_build(const LabFrame *fr,const LabViewOpts *o,int axis,double fraction,bool flip,LabScene *s) {
    memset(s,0,sizeof *s); if(!labscene_supported(fr))return false;
    for(int k=0;k<3;k++)s->lo[k]=INFINITY,s->hi[k]=-INFINITY;
    for(int p=0;p<fr->nparts;p++)for(int i=0;i<fr->parts[p].npoints;i++)for(int k=0;k<3;k++) {
        double x=fr->parts[p].xyz[3*i+k];if(!isfinite(x))goto fail;
        s->lo[k]=fmin(s->lo[k],x);s->hi[k]=fmax(s->hi[k],x);
    }
    const LabPart *vol=labscene_volume(fr); /* a computed volume's box belongs to the scene's bounds */
    if(vol)for(int k=0;k<3;k++) {
        const LabBlock *b=vol->blocks;
        s->lo[k]=fmin(s->lo[k],b->origin[k]);s->hi[k]=fmax(s->hi[k],b->origin[k]+b->dx[k]*b->n[k]);
    }
    if(!isfinite(s->lo[0]))goto fail;
    if(axis>2 || axis< -1 || !isfinite(fraction))goto fail;
    double cut=axis<0?0:s->lo[axis]+fmin(1,fmax(0,fraction))*(s->hi[axis]-s->lo[axis]);
    size_t ct=0,cp=0,cl=0;
    for(int p=0;p<fr->nparts;p++) {
        const LabPart *t=fr->parts+p;const LabField *f=lab_find_field(t,o->field);
        if(t->kind==LAB_BLOCKS)continue;
        if(t->kind==LAB_POINTS) {
            if(!reserve(&s->points,&cp,s->npoints+t->npoints))goto fail;
            for(int i=0;i<t->npoints;i++)if(keep(axis<0?0:t->xyz[3*i+axis],axis,cut,flip))s->points[s->npoints++]=vertex(t,f,i,0,o);
            continue;
        }
        int per=t->cell_type;
        size_t tri_start=s->ntri;
        size_t nf=0,nmax=(size_t)t->ncells*(per==8?6:1);
        if(nmax>SIZE_MAX/sizeof(Face))goto fail;
        Face *faces=malloc(nmax*sizeof *faces);if(nmax && !faces)goto fail;
        static const int H[6][4]={{0,3,2,1},{4,5,6,7},{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7}};
        for(int c=0;c<t->ncells;c++) {
            const int *e=t->conn+per*c;double centre=0;
            for(int i=0;i<per;i++) {if(e[i]<0 || e[i]>=t->npoints){free(faces);goto fail;}if(axis>=0)centre+=t->xyz[3*e[i]+axis]/per;}
            if(!keep(centre,axis,cut,flip))continue;
            if(per==2) {
                if(!reserve(&s->lines,&cl,s->nlines+2)){free(faces);goto fail;}
                for(int k=0;k<2;k++)s->lines[s->nlines++]=vertex(t,f,e[k],c,o);
            } else for(int j=0;j<(per==8?6:1);j++) {
                Face *F=faces+nf++;F->cell=c;
                for(int k=0;k<4;k++)F->v[k]=F->key[k]=e[per==8?H[j][k]:(k<per?k:per-1)];
                sort(F->key);
            }
        }
        if(per==8)qsort(faces,nf,sizeof *faces,compare);
        for(size_t i=0;i<nf;) {
            size_t j=i+1;if(per==8)while(j<nf && compare(faces+i,faces+j)==0)j++;
            if(j==i+1) {
                Face *F=faces+i;int nv=F->v[2]==F->v[3]?3:6;static const int order[6]={0,1,2,0,2,3};
                if(!reserve(&s->tri,&ct,s->ntri+nv)){free(faces);goto fail;}
                for(int k=0;k<nv;k++)s->tri[s->ntri++]=vertex(t,f,F->v[order[k]],F->cell,o);
                if(o->mesh) {
                    int n=nv==3?3:4;if(!reserve(&s->lines,&cl,s->nlines+2*n)){free(faces);goto fail;}
                    for(int k=0;k<n;k++)for(int m=0;m<2;m++) {
                        LabVertex v=vertex(t,NULL,F->v[(k+m)%n],F->cell,o);v.field=-1;s->lines[s->nlines++]=v;
                    }
                }
            }
            i=j;
        }
        free(faces);
        if(per==3){double sp=0;for(int k=0;k<3;k++)sp=fmax(sp,s->hi[k]-s->lo[k]);smooth_normals(s->tri,tri_start,s->ntri,sp);}
    }
    return true;
fail:labscene_free(s);return false;
}

const LabPart *labscene_volume(const LabFrame *fr) {
    for (int p = 0; fr && p < fr->nparts; p++)
        if (volume_part(fr->parts + p)) return fr->parts + p;
    return NULL;
}
