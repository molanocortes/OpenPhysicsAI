/* Criteria fixed before first run: two adjacent unit hexes have 20 exterior triangles;
 * a section retaining either hex has 12, including its newly exposed interface.
 * Nodal and cell fields must retain their own values; sections must not move nodes.
 * Invalid connectivity must fail rather than read outside the stored result. */
#include "../src/lab/labscene.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#define CHECK(c) do { if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);return 1;} } while(0)
int main(void){
    double xyz[36];int at=0;
    for(int x=0;x<3;x++)for(int z=0;z<2;z++)for(int y=0;y<2;y++){xyz[at++]=x;xyz[at++]=y;xyz[at++]=z;}
    int conn[16]={0,4,5,1,2,6,7,3,4,8,9,5,6,10,11,7};
    float values[2]={7,19};LabField f={.name="stress",.location=LAB_AT_CELL,.count=2,.data=values};
    LabPart p={.kind=LAB_CELLS,.npoints=12,.xyz=xyz,.ncells=2,.cell_type=LAB_HEX,.conn=conn,.nfields=1,.fields=&f};
    LabFrame fr={.nparts=1,.parts=&p};LabViewOpts o={.field="stress"};LabScene s;
    CHECK(labscene_build(&fr,&o,-1,.5,false,&s));CHECK(s.ntri==60);CHECK(s.lo[0]==0 && s.hi[0]==2);labscene_free(&s);
    for(int flip=0;flip<2;flip++){
        CHECK(labscene_build(&fr,&o,0,.5,flip,&s));CHECK(s.ntri==36);int face=0;
        for(size_t i=0;i<s.ntri;i++){CHECK(s.tri[i].value==(flip?19:7));CHECK(s.tri[i].p[0]>=(flip?1:0) && s.tri[i].p[0]<=(flip?2:1));face+=s.tri[i].p[0]==1;}
        CHECK(face>=6);labscene_free(&s);
    }
    char err[256];LabWriter *w=lab_create("build/labscene.lab","{\"domain\":\"impact\",\"title\":\"Two cells: section verification\",\"fields\":{\"stress\":\"Pa\"}}",err,sizeof err);
    CHECK(w);for(int k=0;k<2;k++){lab_frame_begin(w,k);lab_part_cells(w,"pair",12,xyz,2,LAB_HEX,conn);lab_field(w,"stress",LAB_AT_CELL,2,values);CHECK(lab_frame_end(w));}CHECK(lab_close(w));
    conn[0]=99;CHECK(!labscene_build(&fr,&o,-1,.5,false,&s));conn[0]=0;
    p.kind=LAB_POINTS;p.npoints=2;p.ncells=0;f.location=LAB_AT_NODE;
    CHECK(labscene_build(&fr,&o,-1,.5,false,&s));CHECK(s.npoints==2 && s.points[0].value==7 && s.points[1].value==19);labscene_free(&s);
    p.kind=LAB_BLOCKS;CHECK(!labscene_supported(&fr));
    puts("labscenetest: exterior faces, section interiors, field identity, invalid connectivity and unsupported geometry PASS");return 0;
}
