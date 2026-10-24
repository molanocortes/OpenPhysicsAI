/* Criteria fixed before first run: every exported volume plane must exactly equal
 * the legacy Ez/material slice at that stored time. Nonzero fields must vary in z.
 * No Maxwell update equations are changed by volume export. */
#include "../src/lab/em/em.h"
#include "../src/lab/acoustic/acoustic.h"
#include "../src/lab/labscene.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) {printf("FAIL line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(void) {
    EmSpec s; em_spec_defaults(&s);
    s.n[0]=24;s.n[1]=20;s.n[2]=16;s.dx=.01;s.pml=0;s.ndip=1;
    s.dip_pos[0][0]=.12;s.dip_pos[0][1]=.1;s.dip_pos[0][2]=.08;
    s.f0=1e9;s.bandwidth=1e9;s.amplitude=1;s.threads=1;
    char err[256];Em *e=em_create(&s,err,sizeof err);CHECK(e);
    for(int k=0;k<35;k++)em_step(e);
    char *h=em_header_json(&s,"Volume export verification");
    LabWriter *w=lab_create("build/labvolume.lab",h,err,sizeof err);CHECK(w);
    lab_frame_begin(w,em_time(e));CHECK(em_write_volume(e,w));CHECK(lab_frame_end(w));CHECK(lab_close(w));
    LabFile *f=lab_open("build/labvolume.lab",err,sizeof err);CHECK(f);
    LabFrame volume;CHECK(lab_read_frame(f,0,&volume,err,sizeof err));CHECK(labscene_volume(&volume));
    double peak=0,variation=0;
    for(int k=0;k<16;k++) {
        w=lab_create("build/labvolume-slice.lab",h,err,sizeof err);CHECK(w);
        lab_frame_begin(w,em_time(e));em_write_frame(e,w,k*s.dx);CHECK(lab_frame_end(w));CHECK(lab_close(w));
        LabFile *sf=lab_open("build/labvolume-slice.lab",err,sizeof err);CHECK(sf);
        LabFrame slice;CHECK(lab_read_frame(sf,0,&slice,err,sizeof err));
        for(int field=0;field<2;field++)for(int q=0;q<480;q++) {
            float value=lab_find_field(volume.parts,field==0?"ez":"material")->data[k*480+q];
            CHECK(value==slice.parts[0].fields[field].data[q]);
            if(field==0){peak=fmax(peak,fabs(value));variation=fmax(variation,fabs(value-lab_find_field(volume.parts,"ez")->data[q]));}
        }
        lab_frame_free(&slice);lab_close_file(sf);
    }
    CHECK(peak>0 && variation>0);
    printf("labvoltest: 15360 scalar samples match 16 original slices exactly; peak %.6g, z variation %.6g PASS\n",peak,variation);
    lab_frame_free(&volume);lab_close_file(f);em_free(e);free(h);
    /* Additional criterion, declared before first run: an empty TF/SF domain has
     * |Ez_scattered| < 1e-5 V/m and reconstructed total matches the independent
     * incident-line sample average within 1e-5 V/m, including injection faces. */
    em_spec_defaults(&s);s.n[0]=s.n[1]=s.n[2]=32;s.dx=.01;s.pml=6;s.threads=1;
    s.plane_wave=true;s.f0=1e9;s.bandwidth=1e9;
    for(int d=0;d<3;d++){s.tfsf_lo[d]=9;s.tfsf_hi[d]=23;}
    e=em_create(&s,err,sizeof err);CHECK(e);for(int i=0;i<100;i++)em_step(e);
    h=em_header_json(&s,"Empty-domain volume identity");w=lab_create("build/labvolume-empty.lab",h,err,sizeof err);CHECK(w);
    lab_frame_begin(w,em_time(e));CHECK(em_write_volume(e,w));CHECK(lab_frame_end(w));CHECK(lab_close(w));
    f=lab_open("build/labvolume-empty.lab",err,sizeof err);CHECK(f);CHECK(lab_read_frame(f,0,&volume,err,sizeof err));
    const LabField *sc=lab_find_field(volume.parts,"ez_scattered"),*tot=lab_find_field(volume.parts,"ez_total");
    double mismatch=0,scmax=0,incmax=0;
    for(int k=8;k<24;k++)for(int j=8;j<24;j++)for(int i=8;i<24;i++){
        double inc=.5*(em_incident_ez(e,i*s.dx)+em_incident_ez(e,(i+1)*s.dx));size_t q=(k*32+j)*32+i;
        mismatch=fmax(mismatch,fabs(tot->data[q]-inc));scmax=fmax(scmax,fabs(sc->data[q]));incmax=fmax(incmax,fabs(inc));
    }
    printf("labvoltest: empty TF/SF volume total error %.3g V/m, scattered %.3g V/m, incident peak %.3g V/m\n",mismatch,scmax,incmax);
    CHECK(incmax>.001 && mismatch<1e-5 && scmax<1e-5);
    lab_frame_free(&volume);lab_close_file(f);em_free(e);free(h);
    /* Acoustic export criterion set before first run: all volume pressure samples
     * exactly match original XY slices; nonzero pressure varies across z. */
    AcSpec as;ac_spec_defaults(&as);as.dx=.05;
    as.size[0]=as.size[1]=as.size[2]=1;as.threads=1;as.nsources=1;
    as.sources[0]=(AcSource){{.5,.5,.5},1,.0001,.0003};
    Acoustic *a=ac_create(&as,err,sizeof err);CHECK(a);
    for(int t=0;t<12;t++)ac_step(a);
    h=ac_header_json(&as,"Acoustic volume identity");
    w=lab_create("build/acoustic-volume.lab",h,err,sizeof err);CHECK(w);
    lab_frame_begin(w,ac_time(a));CHECK(ac_write_volume(a,w));CHECK(lab_frame_end(w));CHECK(lab_close(w));
    f=lab_open("build/acoustic-volume.lab",err,sizeof err);CHECK(f);CHECK(lab_read_frame(f,0,&volume,err,sizeof err));
    CHECK(labscene_volume(&volume));int dims[3];ac_dims(a,dims);peak=variation=0;
    for(int z=0;z<dims[2];z++){
        w=lab_create("build/acoustic-slice.lab",h,err,sizeof err);CHECK(w);
        lab_frame_begin(w,ac_time(a));ac_write_frame(a,w,z*as.dx,-1,-1,0,1);CHECK(lab_frame_end(w));CHECK(lab_close(w));
        LabFile *sf=lab_open("build/acoustic-slice.lab",err,sizeof err);CHECK(sf);
        LabFrame slice;CHECK(lab_read_frame(sf,0,&slice,err,sizeof err));
        int plane=dims[0]*dims[1];
        for(int q=0;q<plane;q++){
            float v=volume.parts[0].fields[0].data[z*plane+q];
            CHECK(v==slice.parts[0].fields[0].data[q]);peak=fmax(peak,fabs(v));
            variation=fmax(variation,fabs(v-volume.parts[0].fields[0].data[q]));
        }
        lab_frame_free(&slice);lab_close_file(sf);
    }
    CHECK(peak>0 && variation>0);
    printf("labvoltest: acoustic %d samples match slices exactly; peak %.6g Pa, z variation %.6g PASS\n",dims[0]*dims[1]*dims[2],peak,variation);
    lab_frame_free(&volume);lab_close_file(f);ac_free(a);free(h);return 0;
}
