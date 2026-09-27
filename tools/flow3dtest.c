/* Criteria fixed before first run:
 * Uniform velocity and density on a 3D periodic lattice retained to 1e-12 for 50 steps.
 * ABC Beltrami flow: curl u=k*u, nonlinear acceleration=grad(|u|^2/2),
 * p=p0-rho*|u|^2/2. Exact incompressible velocity decays as exp(-nu*k*k*t).
 * Low Mach amplitude .003 at 16^3/32^3, nu=.1, matched viscous time t/n^2=.2:
 * vector relative L2 <3% on 16^3 and refinement ratio <.6. Mass drift <1e-12.
 * This verifies periodic fluid evolution, not body forces, turbulence or inlets. */
#include "../src/lab/flow/flow3d.h"
#include "../src/lab/lab_domains.h"
#include "../src/lab/labio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static double decay(int n) {
 Flow3D f;if(!flow3d_create(&f,n,.1))return INFINITY;
 double *rho=malloc(f.count*sizeof(double)),*u=malloc(f.count*3*sizeof(double));
 if(!rho||!u){free(rho);free(u);flow3d_free(&f);return INFINITY;}
 double mass=0,k=2*M_PI/n,A=.003;size_t q=0;
 for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++) {
  u[3*q]=A*(sin(k*z)+cos(k*y));u[3*q+1]=A*(sin(k*x)+cos(k*z));u[3*q+2]=A*(sin(k*y)+cos(k*x));
  double s=0;for(int d=0;d<3;d++)s+=u[3*q+d]*u[3*q+d];rho[q]=1-1.5*(s-3*A*A);mass+=rho[q];
 }
 bool ok=flow3d_initialize(&f,rho,u);int steps=(int)lround(.2*n*n);
 for(int t=0;t<steps&&ok;t++)ok=flow3d_step(&f);
 double factor=exp(-.1*k*k*steps),err=0,norm=0,after=0;
 for(q=0;q<f.count;q++){double r,v[3];flow3d_sample(&f,q,&r,v);after+=r;
  for(int d=0;d<3;d++){double exact=u[3*q+d]*factor,e=v[d]-exact;err+=e*e;norm+=exact*exact;}}
 double drift=fabs(after-mass)/mass;printf("flow3d %d^3: L2 %.9g, mass drift %.9g\n",n,sqrt(err/norm),drift);
 free(rho);free(u);flow3d_free(&f);return ok&&drift<1e-12?sqrt(err/norm):INFINITY;
}
int main(void) {
 Flow3D f;CHECK(flow3d_create(&f,8,.1));double rho[512],u[1536];
 for(int q=0;q<512;q++){rho[q]=1;u[3*q]=.01;u[3*q+1]=-.02;u[3*q+2]=.005;}
 CHECK(flow3d_initialize(&f,rho,u));for(int t=0;t<50;t++)CHECK(flow3d_step(&f));
 for(int q=0;q<512;q++){double r,v[3];flow3d_sample(&f,q,&r,v);CHECK(fabs(r-1)<1e-12);for(int d=0;d<3;d++)CHECK(fabs(v[d]-u[3*q+d])<1e-12);}
 flow3d_free(&f);double a=decay(16),b=decay(32);CHECK(a<.03);CHECK(b<.6*a);
 /* Wall/forcing criterion added before first run: periodic plane Poiseuille,
  * ten fluid cells between halfway walls, nu=.1, acceleration=1e-5.
  * After 4000 steps: relative velocity L2 <2%, transverse velocity <1e-12,
  * fluid mass relative drift <1e-12. No changes to previous criteria. */
 CHECK(flow3d_create(&f,12,.1));double *rr=malloc(f.count*sizeof(double)),*uu=calloc(f.count*3,sizeof(double));CHECK(rr&&uu);
 for(size_t q=0;q<f.count;q++){rr[q]=1;int y=(q/12)%12;f.solid[q]=(y==0||y==11);}
 CHECK(flow3d_initialize(&f,rr,uu));f.acceleration[0]=1e-5;
 for(int t=0;t<4000;t++)CHECK(flow3d_step(&f));
 double error=0,norm=0,mass=0,transverse=0;
 for(size_t q=0;q<f.count;q++)if(!f.solid[q]) {
  double r,v[3];flow3d_sample(&f,q,&r,v);double y=(q/12)%12-.5,exact=1e-5/(2*.1)*y*(10-y);
  error+=(v[0]-exact)*(v[0]-exact);norm+=exact*exact;mass+=r;
  transverse=fmax(transverse,fmax(fabs(v[1]),fabs(v[2])));
 }
 error=sqrt(error/norm);double drift=fabs(mass-1440)/1440;
 printf("flow3d Poiseuille L2 %.9g, transverse %.9g, mass drift %.9g\n",error,transverse,drift);
 CHECK(error<.02);CHECK(transverse<1e-12);CHECK(drift<1e-12);
 free(rr);free(uu);flow3d_free(&f);
 /* New integration criterion before this case's first run: a finite wing
  * produces nonzero spanwise velocity and a stored 3D grid, not an extrusion.
  * This is a dimensionality guard, not a wing-force validation. */
 const char *scenario="{\"domain\":\"flow\",\"model\":\"periodic3d\",\"boundary\":\"periodic\",\"initial_field\":\"uniform\","
  "\"grid\":{\"cells_per_axis\":24,\"cell_m\":0.001},\"time_step_s\":0.001,"
  "\"fluid\":{\"kinematic_viscosity_m2_s\":0.0001,\"source\":\"verification fixture\"},"
  "\"velocity_amplitude_m_s\":0.03,\"body\":{\"shape\":\"finite_naca00\",\"center_m\":[0.007,0.012,0.012],"
  "\"chord_m\":0.012,\"span_m\":0.012,\"angle_deg\":20,\"thickness_ratio\":0.25},"
  "\"run\":{\"end_time_s\":0.02,\"frames\":1}}";
 JsonValue *root=json_parse(scenario,strlen(scenario),NULL,NULL);CHECK(root);
 LabRunInfo info;char errbuf[256];CHECK(lab_run_domain("flow",root,"build/flow3d-wing.lab",true,&info,errbuf,sizeof errbuf));json_free(root);
 LabFile *file=lab_open("build/flow3d-wing.lab",errbuf,sizeof errbuf);CHECK(file);LabFrame frame;
 CHECK(lab_read_frame(file,1,&frame,errbuf,sizeof errbuf));CHECK(frame.parts[0].blocks[0].n[2]==24);
 const LabField *span=lab_find_field(frame.parts,"uy");CHECK(span);double maxspan=0;
 for(size_t q=0;q<span->count;q++)maxspan=fmax(maxspan,fabs(span->data[q]));
 printf("flow3d finite wing spanwise peak %.9g m/s\n",maxspan);CHECK(maxspan>1e-6);
 lab_frame_free(&frame);lab_close_file(file);
 puts("flow3dtest: all criteria PASS");return 0;
}
