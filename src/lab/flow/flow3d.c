#include "flow3d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
/* Same D3Q19 quadrature as the existing tunnel solver, without its float storage
 * or app-specific boundaries. Collision and state arithmetic are double. */
static const int c[19][3]={
 {0,0,0},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
 {1,1,0},{-1,-1,0},{1,-1,0},{-1,1,0},{1,0,1},{-1,0,-1},
 {1,0,-1},{-1,0,1},{0,1,1},{0,-1,-1},{0,1,-1},{0,-1,1}};
static double weight(int d){return d==0?1.0/3:d<7?1.0/18:1.0/36;}
static double equilibrium(int d,double rho,const double *u) {
 double cu=c[d][0]*u[0]+c[d][1]*u[1]+c[d][2]*u[2];
 return weight(d)*rho*(1+3*cu+4.5*cu*cu-1.5*(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]));
}
bool flow3d_create(Flow3D *f,int n,double nu) {
 memset(f,0,sizeof *f);if(n<4||n>128||!isfinite(nu)||nu<=0)return false;
 f->n=n;f->nu=nu;f->count=(size_t)n*n*n;
 f->f=calloc(f->count*19,sizeof(double));f->next=calloc(f->count*19,sizeof(double));
 f->solid=calloc(f->count,1);
 if(!f->f||!f->next||!f->solid){flow3d_free(f);return false;}return true;
}
void flow3d_free(Flow3D *f){free(f->f);free(f->next);free(f->solid);memset(f,0,sizeof *f);}
bool flow3d_initialize(Flow3D *f,const double *rho,const double *u) {
 if(!f||!f->f||!rho||!u)return false;
 for(size_t q=0;q<f->count;q++) {
  if(!isfinite(rho[q])||rho[q]<=0)return false;
  double u2=0;for(int d=0;d<3;d++){if(!isfinite(u[3*q+d]))return false;u2+=u[3*q+d]*u[3*q+d];}
  if(u2>=.04)return false;
 }
 for(size_t q=0;q<f->count;q++)for(int d=0;d<19;d++)f->f[19*q+d]=equilibrium(d,rho[q],u+3*q);
 f->steps=0;return true;
}
void flow3d_sample(const Flow3D *f,size_t q,double *rho,double u[3]) {
 *rho=0;u[0]=u[1]=u[2]=0;
 for(int d=0;d<19;d++){double v=f->f[19*q+d];*rho+=v;for(int a=0;a<3;a++)u[a]+=c[d][a]*v;}
 if(*rho>0)for(int a=0;a<3;a++)u[a]=f->solid[q]?0:u[a]/(*rho)+.5*f->acceleration[a];
}
bool flow3d_step(Flow3D *f) {
 if(!f||!f->f)return false;
 int n=f->n;double omega=1/(.5+3*f->nu);size_t q=0;
 for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++) {
  if(f->solid[q])continue;
  double rho,u[3];flow3d_sample(f,q,&rho,u);
  double u2=u[0]*u[0]+u[1]*u[1]+u[2]*u[2];
  if(!isfinite(rho)||rho<=0||!isfinite(u2)||u2>=.04)return false;
  for(int d=0;d<19;d++) {
   int xx=(x+c[d][0]+n)%n,yy=(y+c[d][1]+n)%n,zz=(z+c[d][2]+n)%n;
   size_t to=((size_t)zz*n+yy)*n+xx;
   double old=f->f[19*q+d];
   double cu=0,force=0;
   for(int a=0;a<3;a++)cu+=c[d][a]*u[a];
   for(int a=0;a<3;a++)force+=(3*(c[d][a]-u[a])+9*cu*c[d][a])*rho*f->acceleration[a];
   double post=old+omega*(equilibrium(d,rho,u)-old)+weight(d)*(1-.5*omega)*force;
   if(f->solid[to])f->next[19*q+(d==0?0:(d%2?d+1:d-1))]=post;
   else f->next[19*to+d]=post;
  }
 }
 double *old=f->f;f->f=f->next;f->next=old;f->steps++;return true;
}
