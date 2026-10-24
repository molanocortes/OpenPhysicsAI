/* Predeclared before first run: double 3D Euler periodic uniform state is
 * preserved to 1e-12. A diagonal linear sound wave, amplitude 1e-6,
 * gamma=1.4, rho=1, c=1, evolves to t=.05. Density-perturbation L2 error
 * <15% on 24^3; 48^3 error <.65 times coarse error. Total conserved
 * quantities drift by <1e-11 relative to max(1, initial magnitude).
 * Recorded first-order baseline: 9.9689% / 5.1099% errors.
 * Added before MUSCL first run: tighten coarse error to <2% and refinement ratio <.45.
 * First MUSCL/minmod run failed tightened coarse criterion: 3.1302%, refined 1.0482%.
 * Replaced minmod by monotonized-central slopes; criteria unchanged.
 * Rusanov Sod refinement failed: .020522445 / .0180773914 (ratio .881).
 * HLLC flux added; shock criteria unchanged.
 * These tests do not verify hypersonic body boundaries. */
#include "../src/lab/gas/gas3d.h"
#include <math.h>
#include <stdio.h>
#define CHECK(x) do{if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
/* ---- exact Riemann solver for the ideal gas (Toro, Riemann Solvers and Numerical Methods, ch. 4) ---- */
typedef struct {
    double r, u, p;
} St;

static void pfun(double p, const St *k, double g, double *f, double *fd) {
    double c = sqrt(g * k->p / k->r);
    if (p <= k->p) {
        double q = p / k->p;
        *f = 2 * c / (g - 1) * (pow(q, (g - 1) / (2 * g)) - 1);
        *fd = 1 / (k->r * c) * pow(q, -(g + 1) / (2 * g));
    } else {
        double A = 2 / ((g + 1) * k->r), B = (g - 1) / (g + 1) * k->p;
        double s = sqrt(A / (p + B));
        *f = (p - k->p) * s;
        *fd = s * (1 - 0.5 * (p - k->p) / (B + p));
    }
}

static St exact_riemann(St L, St R, double g, double xi) {
    double p = 0.5 * (L.p + R.p);
    for (int it = 0; it < 100; it++) {
        double fl, fdl, fr, fdr;
        pfun(p, &L, g, &fl, &fdl);
        pfun(p, &R, g, &fr, &fdr);
        double dp = (fl + fr + R.u - L.u) / (fdl + fdr);
        p -= dp;
        if (p < 1e-12) p = 1e-12;
        if (fabs(dp) < 1e-14 * p) break;
    }
    double fl, fdl, fr, fdr;
    pfun(p, &L, g, &fl, &fdl);
    pfun(p, &R, g, &fr, &fdr);
    double u = 0.5 * (L.u + R.u) + 0.5 * (fr - fl);
    double cl = sqrt(g * L.p / L.r), cr = sqrt(g * R.p / R.r);
    St o;
    if (xi <= u) {
        if (p > L.p) {
            double sl = L.u - cl * sqrt((g + 1) / (2 * g) * p / L.p + (g - 1) / (2 * g));
            if (xi <= sl) return L;
            o.r = L.r * (p / L.p + (g - 1) / (g + 1)) / ((g - 1) / (g + 1) * p / L.p + 1), o.u = u, o.p = p;
            return o;
        }
        double shl = L.u - cl, cs = cl * pow(p / L.p, (g - 1) / (2 * g)), stl = u - cs;
        if (xi <= shl) return L;
        if (xi >= stl) {
            o.r = L.r * pow(p / L.p, 1 / g), o.u = u, o.p = p;
            return o;
        }
        double c = 2 / (g + 1) * (cl + (g - 1) / 2 * (L.u - xi));
        o.u = 2 / (g + 1) * (cl + (g - 1) / 2 * L.u + xi);
        o.r = L.r * pow(c / cl, 2 / (g - 1));
        o.p = L.p * pow(c / cl, 2 * g / (g - 1));
        return o;
    }
    if (p > R.p) {
        double sr = R.u + cr * sqrt((g + 1) / (2 * g) * p / R.p + (g - 1) / (2 * g));
        if (xi >= sr) return R;
        o.r = R.r * (p / R.p + (g - 1) / (g + 1)) / ((g - 1) / (g + 1) * p / R.p + 1), o.u = u, o.p = p;
        return o;
    }
    double shr = R.u + cr, cs = cr * pow(p / R.p, (g - 1) / (2 * g)), str = u + cs;
    if (xi >= shr) return R;
    if (xi <= str) {
        o.r = R.r * pow(p / R.p, 1 / g), o.u = u, o.p = p;
        return o;
    }
    double c = 2 / (g + 1) * (cr - (g - 1) / 2 * (R.u - xi));
    o.u = 2 / (g + 1) * (-cr + (g - 1) / 2 * R.u + xi);
    o.r = R.r * pow(c / cr, 2 / (g - 1));
    o.p = R.p * pow(c / cr, 2 * g / (g - 1));
    return o;
}

/* Additional criterion, before first shock test: Sod at t=.05, central half
 * of a periodic cube (before boundary waves arrive), mean absolute density
 * error <.035 at 32 cubed; 64 cubed error <.8 times coarse error. */
static double shock(int n){
 Gas3D g;if(!gas3d_create(&g,n,1.0/n,1.4))return INFINITY;
 double v[3]={0};
 for(size_t q=0;q<g.count;q++){int x=q%n;gas3d_set(&g,q,x<n/2?1:.125,v,x<n/2?1:.1);}
 while(g.time<.05-1e-14)if(!gas3d_step(&g,.05-g.time)){gas3d_free(&g);return INFINITY;}
 double err=0;int count=0;St L={1,0,1},R={.125,0,.1};
 for(int x=n/4;x<3*n/4;x++){
  size_t q=((size_t)(n/2)*n+n/2)*n+x;
  St exact=exact_riemann(L,R,1.4,((x+.5)/n-.5)/g.time);err+=fabs(g.u[5*q]-exact.r);count++;
 }
 gas3d_free(&g);printf("gas3d Sod %d^3 central density L1 %.9g\n",n,err/count);return err/count;
}
static double wave(int n){
 Gas3D g;if(!gas3d_create(&g,n,1.0/n,1.4))return INFINITY;
 size_t q=0;double initial[5]={0},amp=1e-6;
 for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++){
  double delta=amp*sin(2*M_PI*(x+y+z+1.5)/n),v[3]={delta/sqrt(3),delta/sqrt(3),delta/sqrt(3)};
  gas3d_set(&g,q,1+delta,v,1/1.4+delta);for(int d=0;d<5;d++)initial[d]+=g.u[5*q+d];
 }
 bool ok=true;while(g.time<.05-1e-14)if(!gas3d_step(&g,.05-g.time)){ok=false;break;}
 double err=0,norm=0,total[5]={0};q=0;
 for(int z=0;z<n;z++)for(int y=0;y<n;y++)for(int x=0;x<n;x++,q++){
  double exact=amp*sin(2*M_PI*((x+y+z+1.5)/n-sqrt(3)*g.time)),e=g.u[5*q]-1-exact;err+=e*e;norm+=exact*exact;
  for(int d=0;d<5;d++)total[d]+=g.u[5*q+d];
 }
 double drift=0;for(int d=0;d<5;d++)drift=fmax(drift,fabs(total[d]-initial[d])/fmax(1,fabs(initial[d])));
 double error=sqrt(err/norm);printf("gas3d diagonal wave %d^3 L2 %.9g, conservation %.9g\n",n,error,drift);
 gas3d_free(&g);return ok&&drift<1e-11?error:INFINITY;
}
int main(void){
 Gas3D g;CHECK(gas3d_create(&g,8,.1,1.4));double v[3]={.2,-.1,.3};for(size_t q=0;q<g.count;q++)gas3d_set(&g,q,1,v,1);
 for(int t=0;t<20;t++)CHECK(gas3d_step(&g,.01)>0);
 for(size_t q=0;q<g.count;q++){double r,u[3],p;CHECK(gas3d_primitive(&g,q,&r,u,&p));CHECK(fabs(r-1)<1e-12&&fabs(p-1)<1e-12);for(int d=0;d<3;d++)CHECK(fabs(u[d]-v[d])<1e-12);}
 gas3d_free(&g);double a=wave(24),b=wave(48);CHECK(a<.02);CHECK(b<.45*a);double c=shock(32),d=shock(64);CHECK(c<.035);CHECK(d<.8*c);puts("gas3dtest: all criteria PASS");return 0;
}
