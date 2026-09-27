/* Criteria declared before first run: kernel integral preserves reference particle
 * volume within 2%; a uniform lattice has interior coverage within 2% of one;
 * a constant scalar is reproduced within 1e-5 at every supported grid sample.
 * This verifies reconstruction, not a new water-physics model. */
#include "../src/lab/labwater.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#define CHECK(x) do{if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
    const int side=12,n=side*side*side;double dx=.02;
    double *xyz=malloc(n*3*sizeof(double));float *data=malloc(n*sizeof(float));int at=0;
    for(int z=0;z<side;z++)for(int y=0;y<side;y++)for(int x=0;x<side;x++,at++){
        xyz[3*at]=(x+.5)*dx;xyz[3*at+1]=(y+.5)*dx;xyz[3*at+2]=(z+.5)*dx;data[at]=3.25f;
    }
    LabField f={.name="speed",.location=LAB_AT_NODE,.count=n,.data=data};
    LabPart p={.kind=LAB_POINTS,.npoints=n,.xyz=xyz,.nfields=1,.fields=&f};LabFrame in={.nparts=1,.parts=&p},out;
    CHECK(labwater_volume(&in,dx,"speed",&out));LabPart *v=out.parts;LabBlock *b=v->blocks;
    double integral=0,inner=0;int ni=0;
    for(int k=0;k<b->n[2];k++)for(int j=0;j<b->n[1];j++)for(int i=0;i<b->n[0];i++){
        size_t q=(k*b->n[1]+j)*b->n[0]+i;double c=v->fields[1].data[q];integral+=c*pow(dx*.5,3);
        if(c>1e-10)CHECK(fabs(v->fields[0].data[q]-3.25)<1e-5);
        double x=b->origin[0]+(i+.5)*b->dx[0],y=b->origin[1]+(j+.5)*b->dx[1],z=b->origin[2]+(k+.5)*b->dx[2];
        if(x>.08 && x<.16 && y>.08 && y<.16 && z>.08 && z<.16){inner+=c;ni++;}
    }
    double error=fabs(integral/(n*dx*dx*dx)-1);CHECK(error<.02);CHECK(ni && fabs(inner/ni-1)<.02);
    printf("labwatertest: volume error %.6g, interior coverage %.6g, constant field reproduced PASS\n",error,inner/ni);
    char err[256];LabWriter *w=lab_create("build/labwater.lab","{\"domain\":\"water\",\"title\":\"Water reconstruction fixture\",\"particle_spacing_m\":0.02,\"fields\":{\"speed\":\"m/s\"}}",err,sizeof err);CHECK(w);
    lab_frame_begin(w,0);lab_part_points(w,"samples",n,xyz);lab_field(w,"speed",LAB_AT_NODE,n,data);CHECK(lab_frame_end(w));CHECK(lab_close(w));
    lab_frame_free(&out);free(xyz);free(data);return 0;
}
