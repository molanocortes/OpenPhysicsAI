#include "flow3d.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "../../core/json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool lab_run_flow3d(const JsonValue *root,const char *out,bool quiet,LabRunInfo *info,char *err,size_t el) {
 const JsonValue *grid=json_get(root,"grid"),*fluid=json_get(root,"fluid"),*run=json_get(root,"run");
 const char *initial=json_get_str(root,"initial_field","");
 bool abc=!strcmp(initial,"abc"),uniform=!strcmp(initial,"uniform");
 int n=(int)json_get_int(grid,"cells_per_axis",0),frames=(int)json_get_int(run,"frames",0);
 double dx=json_get_num(grid,"cell_m",0),dt=json_get_num(root,"time_step_s",0),
  nu=json_get_num(fluid,"kinematic_viscosity_m2_s",0),A=json_get_num(root,"velocity_amplitude_m_s",0),
  end=json_get_num(run,"end_time_s",0);
 if(!isfinite(dx)||dx<=0||!isfinite(dt)||dt<=0||!isfinite(nu)||nu<=0||!isfinite(A)||A<=0||
    !isfinite(end)||end<=0||end/dt>10000000||frames<1||frames>10000||!json_get_str(fluid,"source",NULL)||
    strcmp(json_get_str(root,"boundary",""),"periodic")||(!abc&&!uniform)) {
  snprintf(err,el,"periodic3d requires positive SI spacing/time step/viscosity/amplitude/end time, sourced fluid, periodic boundary and abc initial_field");return false;
 }
 double a=A*dt/dx,nulb=nu*dt/(dx*dx);
 if(!isfinite(a)||a>(abc?.04:.1)||!isfinite(nulb)){snprintf(err,el,"Amplitude exceeds low-Mach limit (.04 ABC, .1 uniform); reduce time_step_s");return false;}
 Flow3D f;if(!flow3d_create(&f,n,nulb)){snprintf(err,el,"cannot create 3D flow grid (4..128 cells per axis)");return false;}
 double *rho=malloc(f.count*sizeof(double)),*u=malloc(f.count*3*sizeof(double));
 float *values=malloc(f.count*sizeof(float));
 if(!rho||!u||!values){free(rho);free(u);free(values);flow3d_free(&f);snprintf(err,el,"cannot allocate flow output");return false;}
 size_t q=0;double k=2*M_PI/n;
 for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++) {
  u[3*q]=a*(sin(k*z)+cos(k*y));u[3*q+1]=a*(sin(k*x)+cos(k*z));u[3*q+2]=a*(sin(k*y)+cos(k*x));
  if(uniform){u[3*q]=a;u[3*q+1]=u[3*q+2]=0;}
  double s=0;for(int d=0;d<3;d++)s+=u[3*q+d]*u[3*q+d];rho[q]=abc?1-1.5*(s-3*a*a):1;
 }
 const JsonValue *body=json_get(root,"body");
 if(body) {
  double center[3],chord=json_get_num(body,"chord_m",0),span=json_get_num(body,"span_m",0),
   angle=json_get_num(body,"angle_deg",NAN)*M_PI/180,thick=json_get_num(body,"thickness_ratio",0);
  if(strcmp(json_get_str(body,"shape",""),"finite_naca00")||!json_get_numbers(json_get(body,"center_m"),center,3)||
     !isfinite(chord)||chord<=0||chord>=n*dx||!isfinite(span)||span<=0||span>=n*dx||
     !isfinite(angle)||!isfinite(thick)||thick<=0||thick>.4) {
   snprintf(err,el,"body requires finite_naca00, center_m, chord_m, span_m, angle_deg and thickness_ratio");
   free(rho);free(u);free(values);flow3d_free(&f);return false;
  }
  for(int d=0;d<3;d++)if(!isfinite(center[d])||center[d]<=0||center[d]>=n*dx) {
   snprintf(err,el,"body center outside grid");free(rho);free(u);free(values);flow3d_free(&f);return false;
  }
  size_t solids=0;q=0;
  for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++) {
   double X=(x+.5)*dx-center[0],Z=(z+.5)*dx-center[2];
   double localx=(X*cos(angle)-Z*sin(angle))/chord+.25,
    localz=X*sin(angle)+Z*cos(angle);
   if(fabs((y+.5)*dx-center[1])<span*.5&&localx>0&&localx<1) {
    double t=5*thick*chord*(.2969*sqrt(localx)-.1260*localx-.3516*localx*localx+
             .2843*localx*localx*localx-.1036*localx*localx*localx*localx);
    if(fabs(localz)<t){f.solid[q]=1;solids++;u[3*q]=u[3*q+1]=u[3*q+2]=0;}
   }
  }
  if(!solids){snprintf(err,el,"body unresolved on this grid");free(rho);free(u);free(values);flow3d_free(&f);return false;}
 }
 bool ok=flow3d_initialize(&f,rho,u);
 JsonValue *meta=json_object();ok=ok&&meta&&json_set_string(meta,"domain","flow")&&
  json_set_string(meta,"title",json_get_str(root,"title","3D periodic flow"))&&json_set(meta,"scenario",json_clone(root));
 JsonValue *fields=json_set_object(meta,"fields");
 const char *names[]={"speed","ux","uy","uz","vorticity"};
 for(int d=0;d<5;d++)ok=ok&&json_set_string(fields,names[d],d==4?"1/s":"m/s");
 char *header=ok?json_dump(meta,0,NULL,NULL):NULL;json_free(meta);
 LabWriter *w=header?lab_create(out,header,err,el):NULL;free(header);
 if(!w){free(rho);free(u);free(values);flow3d_free(&f);return false;}
 LabBlock b={{n,n,n},0,LAB_PLANE_XY,{0,0,0},{dx,dx,dx}};
 for(int frame=0;frame<=frames&&ok;frame++) {
  long target=lround(end*frame/(frames*dt));
  while(f.steps<target&&ok)ok=flow3d_step(&f);
  if(!ok){snprintf(err,el,"3D flow exceeded low-Mach stability bound at step %ld",f.steps);break;}
  for(q=0;q<f.count;q++){flow3d_sample(&f,q,rho+q,u+3*q);for(int d=0;d<3;d++)u[3*q+d]*=dx/dt;}
  lab_frame_begin(w,f.steps*dt);lab_part_blocks(w,"flow_volume",1,&b);
  for(int field=0;field<5;field++) {
   q=0;
   for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++) {
    double *v=u+3*q;
    if(field==0)values[q]=(float)sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    else if(field<4)values[q]=(float)v[field-1];
    else {
     size_t xp=((size_t)z*n+y)*n+(x+1)%n,xm=((size_t)z*n+y)*n+(x+n-1)%n,
      yp=((size_t)z*n+(y+1)%n)*n+x,ym=((size_t)z*n+(y+n-1)%n)*n+x,
      zp=((size_t)((z+1)%n)*n+y)*n+x,zm=((size_t)((z+n-1)%n)*n+y)*n+x;
     double wx=(u[3*yp+2]-u[3*ym+2]-u[3*zp+1]+u[3*zm+1])/(2*dx),
      wy=(u[3*zp]-u[3*zm]-u[3*xp+2]+u[3*xm+2])/(2*dx),
      wz=(u[3*xp+1]-u[3*xm+1]-u[3*yp]+u[3*ym])/(2*dx);
     values[q]=(float)sqrt(wx*wx+wy*wy+wz*wz);
    }
   }
   lab_field(w,names[field],LAB_AT_CELL,f.count,values);
  }
  if(body){for(q=0;q<f.count;q++)values[q]=f.solid[q]?1:0;lab_field(w,"material",LAB_AT_CELL,f.count,values);}
  ok=lab_frame_end(w);
  if(!quiet)fprintf(stderr,"  3D flow frame %d/%d, step %ld\n",frame,frames,f.steps);
 }
 info->frames=lab_frames_written(w);info->steps=f.steps;
 snprintf(info->diagnostics,sizeof info->diagnostics,"{\"end_time_s\":%.9g,\"cells\":%zu,\"nu_lattice\":%.9g}",f.steps*dt,f.count,nulb);
 if(!lab_close(w))ok=false;free(rho);free(u);free(values);flow3d_free(&f);return ok;
}
