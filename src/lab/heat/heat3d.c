#include "heat3d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool heat3d_create(Heat3D *h,const int n[3],double dx) {
    memset(h,0,sizeof *h);
    if(!isfinite(dx)||dx<=0) return false;
    size_t count=1;
    for(int d=0;d<3;d++) {
        if(n[d]<2||n[d]>512||count>8000000u/(size_t)n[d]) return false;
        count*=n[d]; h->n[d]=n[d];
    }
    h->count=count; h->dx=dx;
    h->T=calloc(count,sizeof(double)); h->next=calloc(count,sizeof(double));
    h->capacity=calloc(count,sizeof(double)); h->conductivity=calloc(count,sizeof(double));
    h->source=calloc(count,sizeof(double));
    if(!h->T||!h->next||!h->capacity||!h->conductivity||!h->source){heat3d_free(h);return false;}
    return true;
}
void heat3d_free(Heat3D *h) {
    free(h->T);free(h->next);free(h->capacity);free(h->conductivity);free(h->source);
    memset(h,0,sizeof *h);
}
static double face(double a,double b){return 2*(a/(a+b))*b;}
static int neighbours(const Heat3D *h,int x,int y,int z,size_t q,size_t nb[6]) {
    int c=0;size_t row=h->n[0],plane=row*h->n[1];
    if(x)nb[c++]=q-1;if(x+1<h->n[0])nb[c++]=q+1;
    if(y)nb[c++]=q-row;if(y+1<h->n[1])nb[c++]=q+row;
    if(z)nb[c++]=q-plane;if(z+1<h->n[2])nb[c++]=q+plane;
    return c;
}
double heat3d_step(Heat3D *h,double max_dt) {
    if(!h||!h->count||!isfinite(max_dt)||max_dt<=0)return 0;
    for(size_t q=0;q<h->count;q++)
        if(!isfinite(h->T[q])||!isfinite(h->source[q])||!isfinite(h->capacity[q])||
           !isfinite(h->conductivity[q])||h->capacity[q]<=0||h->conductivity[q]<=0)return 0;
    double dt=max_dt,dx2=h->dx*h->dx;
    size_t q=0;
    for(int z=0;z<h->n[2];z++)for(int y=0;y<h->n[1];y++)for(int x=0;x<h->n[0];x++,q++) {
        size_t nb[6];int n=neighbours(h,x,y,z,q,nb);double sum=0;
        for(int d=0;d<n;d++)sum+=face(h->conductivity[q],h->conductivity[nb[d]]);
        dt=fmin(dt,.9*h->capacity[q]*dx2/sum);
    }
    if(!isfinite(dt)||dt<=0)return 0;
    q=0;
    for(int z=0;z<h->n[2];z++)for(int y=0;y<h->n[1];y++)for(int x=0;x<h->n[0];x++,q++) {
        size_t nb[6];int n=neighbours(h,x,y,z,q,nb);double flux=0;
        for(int d=0;d<n;d++)flux+=face(h->conductivity[q],h->conductivity[nb[d]])*(h->T[nb[d]]-h->T[q]);
        h->next[q]=h->T[q]+dt*(flux/dx2+h->source[q])/h->capacity[q];
        if(!isfinite(h->next[q]))return 0;
    }
    double *old=h->T;h->T=h->next;h->next=old;h->time+=dt;return dt;
}
double heat3d_energy(const Heat3D *h) {
    double e=0;for(size_t q=0;q<h->count;q++)e+=h->capacity[q]*h->T[q];
    return e*h->dx*h->dx*h->dx;
}
