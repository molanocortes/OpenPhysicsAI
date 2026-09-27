#include "gas3d.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "../../core/json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool lab_run_gas3d(const JsonValue *root,const char *out,bool quiet,LabRunInfo *info,char *err,size_t el){
 const JsonValue *grid=json_get(root,"grid"),*initial=json_get(root,"initial"),*run=json_get(root,"run");
 int n=(int)json_get_int(grid,"cells_per_axis",0),frames=(int)json_get_int(run,"frames",0);
 double dx=json_get_num(grid,"cell_m",0),gamma=json_get_num(initial,"gamma",0),rho=json_get_num(initial,"density_kg_m3",0),
  p=json_get_num(initial,"pressure_pa",0),amplitude=json_get_num(initial,"pulse_pressure_pa",NAN),
  width=json_get_num(initial,"pulse_width_m",0),center[3],end=json_get_num(run,"end_time_s",0);
 if(!isfinite(rho)||rho<=0||!isfinite(p)||p<=0||!isfinite(amplitude)||amplitude<0||!isfinite(width)||width<=0||
    !json_get_numbers(json_get(initial,"center_m"),center,3)||!json_get_str(initial,"source",NULL)||
    !isfinite(end)||end<=0||frames<1||frames>10000||strcmp(json_get_str(root,"boundary",""),"periodic")){
  snprintf(err,el,"euler3d requires explicit periodic boundary, sourced positive gas state, Gaussian pulse and run controls");return false;
 }
 Gas3D g;if(!gas3d_create(&g,n,dx,gamma)){snprintf(err,el,"invalid 3D gas grid or gamma");return false;}
 for(int d=0;d<3;d++)if(!isfinite(center[d])||center[d]<0||center[d]>n*dx){snprintf(err,el,"pulse center outside grid");gas3d_free(&g);return false;}
 size_t q=0;double velocity[3]={0};
 for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++){
  double X=(x+.5)*dx-center[0],Y=(y+.5)*dx-center[1],Z=(z+.5)*dx-center[2];
  double pressure=p+amplitude*exp(-(X*X+Y*Y+Z*Z)/(2*width*width));
  gas3d_set(&g,q,rho*pow(pressure/p,1/gamma),velocity,pressure);
 }
 JsonValue *meta=json_object();bool ok=meta&&json_set_string(meta,"domain","gas")&&
  json_set_string(meta,"title",json_get_str(root,"title","3D ideal gas"))&&json_set(meta,"scenario",json_clone(root));
 JsonValue *fields=json_set_object(meta,"fields");const char *names[]={"pressure","pressure_perturbation","density","speed"};
 const char *units[]={"Pa","Pa","kg/m3","m/s"};for(int d=0;d<4;d++)ok=ok&&json_set_string(fields,names[d],units[d]);
 char *header=ok?json_dump(meta,0,NULL,NULL):NULL;json_free(meta);LabWriter *w=header?lab_create(out,header,err,el):NULL;free(header);
 float *data=malloc(g.count*4*sizeof(float));
 if(!w||!data){if(w)lab_close(w);free(data);gas3d_free(&g);snprintf(err,el,"cannot create gas output");return false;}
 LabBlock b={{n,n,n},0,LAB_PLANE_XY,{0,0,0},{dx,dx,dx}};long steps=0;
 for(int f=0;f<=frames&&ok;f++){
  double target=end*f/frames;
  while(g.time<target-1e-13*fmax(1,target)){if(!gas3d_step(&g,target-g.time)){snprintf(err,el,"nonphysical gas state at %.9g s",g.time);ok=false;break;}steps++;}
  if(!ok)break;
  for(q=0;q<g.count;q++){double r,v[3],P;gas3d_primitive(&g,q,&r,v,&P);data[q]=P;data[g.count+q]=P-p;data[2*g.count+q]=r;data[3*g.count+q]=sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);}
  lab_frame_begin(w,g.time);lab_part_blocks(w,"gas_volume",1,&b);for(int d=0;d<4;d++)lab_field(w,names[d],LAB_AT_CELL,g.count,data+d*g.count);
  ok=lab_frame_end(w);if(!quiet)fprintf(stderr,"  3D gas frame %d/%d, t %.6g s\n",f,frames,g.time);
 }
 info->frames=lab_frames_written(w);info->steps=steps;snprintf(info->diagnostics,sizeof info->diagnostics,"{\"end_time_s\":%.9g,\"cells\":%zu}",g.time,g.count);
 if(!lab_close(w))ok=false;free(data);gas3d_free(&g);return ok;
}
