#include "gas3d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
bool gas3d_create(Gas3D *g,int n,double dx,double gamma){
 memset(g,0,sizeof *g);if(n<4||n>128||!isfinite(dx)||dx<=0||!isfinite(gamma)||gamma<=1)return false;
 g->n=n;g->count=(size_t)n*n*n;g->dx=dx;g->gamma=gamma;
 g->u=calloc(g->count*5,sizeof(double));g->next=calloc(g->count*5,sizeof(double));
 g->stage=calloc(g->count*5,sizeof(double));
 if(!g->u||!g->next||!g->stage){gas3d_free(g);return false;}return true;
}
void gas3d_free(Gas3D *g){free(g->u);free(g->next);free(g->stage);memset(g,0,sizeof *g);}
void gas3d_set(Gas3D *g,size_t q,double rho,const double v[3],double p){
 double *u=g->u+5*q;u[0]=rho;double kinetic=0;
 for(int d=0;d<3;d++){u[1+d]=rho*v[d];kinetic+=.5*rho*v[d]*v[d];}u[4]=p/(g->gamma-1)+kinetic;
}
static bool primitive(const double *u,double gamma,double *rho,double v[3],double *p){
 *rho=u[0];if(!isfinite(*rho)||*rho<=0)return false;double kinetic=0;
 for(int d=0;d<3;d++){v[d]=u[d+1]/(*rho);kinetic+=.5*u[d+1]*v[d];if(!isfinite(v[d]))return false;}
 *p=(gamma-1)*(u[4]-kinetic);return isfinite(*p)&&*p>0;
}
bool gas3d_primitive(const Gas3D *g,size_t q,double *rho,double v[3],double *p){return primitive(g->u+5*q,g->gamma,rho,v,p);}
static void flux(const double *u,double p,const double *v,int axis,double *f){
 f[0]=u[axis+1];for(int d=0;d<3;d++)f[d+1]=u[d+1]*v[axis]+(d==axis?p:0);f[4]=(u[4]+p)*v[axis];
}
/* HLLC restores the contact wave missing from the diffusive Rusanov flux.
 * Invalid star states fall back to Rusanov without modifying conserved data. */
static void riemann(const double *L,const double *R,double rl,double rr,const double *vl,const double *vr,
                    double pl,double pr,double gamma,int d,double result[5]){
 double fl[5],fr[5];flux(L,pl,vl,d,fl);flux(R,pr,vr,d,fr);
 double cl=sqrt(gamma*pl/rl),cr=sqrt(gamma*pr/rr),sl=fmin(vl[d]-cl,vr[d]-cr),sr=fmax(vl[d]+cl,vr[d]+cr);
 if(sl>=0){memcpy(result,fl,5*sizeof(double));return;}if(sr<=0){memcpy(result,fr,5*sizeof(double));return;}
 double denominator=rl*(sl-vl[d])-rr*(sr-vr[d]);
 double sm=(pr-pl+rl*vl[d]*(sl-vl[d])-rr*vr[d]*(sr-vr[d]))/denominator;
 const double *U=sm>=0?L:R,*v=sm>=0?vl:vr,*F=sm>=0?fl:fr;
 double rho=sm>=0?rl:rr,p=sm>=0?pl:pr,S=sm>=0?sl:sr,star[5];
 star[0]=rho*(S-v[d])/(S-sm);
 for(int a=0;a<3;a++)star[a+1]=star[0]*(a==d?sm:v[a]);
 star[4]=star[0]*(U[4]/rho+(sm-v[d])*(sm+p/(rho*(S-v[d]))));
 double rs,vs[3],ps;
 if(isfinite(sm)&&sm>=sl&&sm<=sr&&primitive(star,gamma,&rs,vs,&ps)){
  for(int j=0;j<5;j++)result[j]=F[j]+S*(star[j]-U[j]);return;
 }
 double a=fmax(fabs(vl[d])+cl,fabs(vr[d])+cr);
 for(int j=0;j<5;j++)result[j]=.5*(fl[j]+fr[j]-a*(R[j]-L[j]));
}
static size_t adjacent(int n,int x,int y,int z,int axis,int sign){
 int c[3]={x,y,z};c[axis]=(c[axis]+sign+n)%n;return ((size_t)c[2]*n+c[1])*n+c[0];
}
static double limited(double a,double b){return a*b<=0?0:copysign(fmin(.5*fabs(a+b),2*fmin(fabs(a),fabs(b))),a);}
static void reconstruct(const Gas3D *g,const double *state,int x,int y,int z,int axis,int direction,double out[5]){
 int n=g->n;size_t q=((size_t)z*n+y)*n+x,lo=adjacent(n,x,y,z,axis,-1),hi=adjacent(n,x,y,z,axis,1);
 for(int j=0;j<5;j++)out[j]=state[5*q+j]+.5*direction*limited(state[5*q+j]-state[5*lo+j],state[5*hi+j]-state[5*q+j]);
 double rho,v[3],p;if(!primitive(out,g->gamma,&rho,v,&p))memcpy(out,state+5*q,5*sizeof(double));
}
static bool advance(const Gas3D *g,const double *state,double *out,double dt){
 memcpy(out,state,g->count*5*sizeof(double));int n=g->n;size_t q=0;
 for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++){
  for(int d=0;d<3;d++){
   int c[3]={x,y,z};c[d]=(c[d]+1)%n;size_t r=((size_t)c[2]*n+c[1])*n+c[0];
   double left[5],right[5],rl,vl[3],pl,rr,vr[3],pr;
   reconstruct(g,state,x,y,z,d,1,left);reconstruct(g,state,c[0],c[1],c[2],d,-1,right);
   if(!primitive(left,g->gamma,&rl,vl,&pl)||!primitive(right,g->gamma,&rr,vr,&pr))return false;
   double f[5];riemann(left,right,rl,rr,vl,vr,pl,pr,g->gamma,d,f);
   for(int j=0;j<5;j++){double transfer=dt/g->dx*f[j];out[5*q+j]-=transfer;out[5*r+j]+=transfer;}
  }
 }
 for(q=0;q<g->count;q++){double r,v[3],p;if(!primitive(out+5*q,g->gamma,&r,v,&p))return false;}return true;
}
double gas3d_step(Gas3D *g,double max_dt){
 if(!g||!g->u||!isfinite(max_dt)||max_dt<=0)return 0;
 double speed=0;
 for(size_t q=0;q<g->count;q++){double rho,v[3],p;if(!gas3d_primitive(g,q,&rho,v,&p))return 0;
  double c=sqrt(g->gamma*p/rho);speed=fmax(speed,fabs(v[0])+fabs(v[1])+fabs(v[2])+3*c);}
 double dt=fmin(max_dt,.4*g->dx/speed);if(!isfinite(dt)||dt<=0)return 0;
 if(!advance(g,g->u,g->stage,dt)||!advance(g,g->stage,g->next,dt))return 0;
 for(size_t q=0;q<g->count*5;q++)g->next[q]=.5*(g->u[q]+g->next[q]);
 double *old=g->u;g->u=g->next;g->next=old;g->time+=dt;return dt;
}
