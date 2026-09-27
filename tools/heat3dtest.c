/* Criteria recorded before first run:
 * 1. Insulating cube exact mode T=300+cos(pi*x)cos(pi*y)cos(pi*z)
 *    exp(-3*pi*pi*alpha*t), alpha=.1, t=.1. Relative mode L2 error
 *    <1% at 16^3; 32^3 error less than 0.3 times 16^3 error.
 * 2. Heterogeneous conductivity/capacity: insulated source-free energy
 *    conserved to 1e-12 relative after 100 steps, no new extrema.
 * 3. Invalid material rejected without advancing time or temperature. */
#include "../src/lab/heat/heat3d.h"
#include <math.h>
#include <stdio.h>
#define CHECK(x) do{if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static double mode(int n) {
    Heat3D h;int dims[3]={n,n,n};if(!heat3d_create(&h,dims,1.0/n))return INFINITY;
    size_t q=0;
    for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++) {
        h.capacity[q]=1;h.conductivity[q]=.1;
        h.T[q]=300+cos(M_PI*(x+.5)/n)*cos(M_PI*(y+.5)/n)*cos(M_PI*(z+.5)/n);
    }
    while(h.time<.1-1e-14)if(!heat3d_step(&h,fmin(.02/(n*n),.1-h.time))){heat3d_free(&h);return INFINITY;}
    double err=0,norm=0; q=0;
    for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++) {
        double v=cos(M_PI*(x+.5)/n)*cos(M_PI*(y+.5)/n)*cos(M_PI*(z+.5)/n)*exp(-3*M_PI*M_PI*.1*.1);
        double e=h.T[q]-300-v;err+=e*e;norm+=v*v;
    }
    heat3d_free(&h);return sqrt(err/norm);
}
int main(void) {
    double a=mode(16),b=mode(32);printf("heat3d mode L2: 16^3 %.9g, 32^3 %.9g, ratio %.6g\n",a,b,b/a);
    CHECK(a<.01);CHECK(b<.3*a);
    Heat3D h;int dims[3]={12,10,8};CHECK(heat3d_create(&h,dims,.1));
    for(size_t q=0;q<h.count;q++){h.capacity[q]=1+(q%3);h.conductivity[q]=.1+(q%7);h.T[q]=280+(q%41);}
    double initial=heat3d_energy(&h);
    for(int i=0;i<100;i++)CHECK(heat3d_step(&h,1)>0);
    double drift=fabs(heat3d_energy(&h)-initial)/initial;printf("heat3d heterogeneous energy relative drift %.9g\n",drift);CHECK(drift<1e-12);
    for(size_t q=0;q<h.count;q++)CHECK(h.T[q]>=280&&h.T[q]<=320);
    double time=h.time,T=h.T[0];h.capacity[0]=0;CHECK(heat3d_step(&h,1)==0);CHECK(h.time==time&&h.T[0]==T);
    heat3d_free(&h);puts("heat3dtest: all criteria PASS");return 0;
}
