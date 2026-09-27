/* Criteria before first run of the MFEM backend:
 * Homogeneous applied field: relative vector error <1e-10 on 8^3 cells.
 * Uniformly magnetized sphere (mu_r=1): mean central Bz=2 Br/3, <8%
 * relative error at 16^3; 32^3 error <.75 coarse error. Exact isolated-sphere
 * potential is imposed at the exterior boundary to exclude truncation error.
 * Every linear solve must converge; relative residual <1e-10.
 * No motor torque, currents or saturation are verified here. */
#include "../src/lab/magnet/magnet3d.h"
#include <cmath>
#include <cstdio>
#define CHECK(x) do{if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static double sphere(int n){
 Magnet3DSpec s={};s.n=n;s.dx=1.0/n;s.radius=.2;s.mu_r=1;s.exact_sphere_boundary=true;
 for(int d=0;d<3;d++)s.center[d]=.5;s.remanence[2]=1;
 Magnet3DResult r={};char err[256];if(!magnet3d_solve(&s,&r,err,sizeof err)){std::puts(err);return INFINITY;}
 double average=0;int count=0;size_t q=0;
 for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++){
  double X=(x+.5)/n-.5,Y=(y+.5)/n-.5,Z=(z+.5)/n-.5;
  if(X*X+Y*Y+Z*Z<.01){average+=r.B[3*q+2];count++;}
 }
 average/=count;double error=std::fabs(average/(2.0/3)-1);
 std::printf("magnet3d sphere %d^3: central B %.12g T, relative error %.9g, residual %.3g, iterations %d\n",n,average,error,r.relative_residual,r.iterations);
 if(r.relative_residual>=1e-10)error=INFINITY;magnet3d_free(&r);return error;
}
int main(){
 Magnet3DSpec s={};s.n=8;s.dx=.125;s.radius=.2;s.mu_r=1;
 for(int d=0;d<3;d++)s.center[d]=.5;
 s.applied[0]=.1;s.applied[1]=-.2;s.applied[2]=.3;
 Magnet3DResult r={};char err[256];bool ok=magnet3d_solve(&s,&r,err,sizeof err);if(!ok)std::puts(err);CHECK(ok);
 double error=0;for(int q=0;q<512;q++)for(int d=0;d<3;d++)error=std::fmax(error,std::fabs(r.B[3*q+d]-s.applied[d]));
 std::printf("magnet3d uniform max error %.9g T, residual %.3g\n",error,r.relative_residual);
 CHECK(error<1e-10);CHECK(r.relative_residual<1e-10);magnet3d_free(&r);
 double a=sphere(16),b=sphere(32);CHECK(a<.08);CHECK(b<.75*a);
 std::puts("magnet3dtest: all criteria PASS");return 0;
}
