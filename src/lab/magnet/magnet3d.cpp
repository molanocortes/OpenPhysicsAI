#include "magnet3d.h"
#include "mfem.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
using namespace mfem;
static_assert(sizeof(real_t)==sizeof(double),"MFEM must use double precision");
extern "C" void magnet3d_free(Magnet3DResult *r){std::free(r->B);std::memset(r,0,sizeof *r);}
extern "C" bool magnet3d_solve(const Magnet3DSpec *s,Magnet3DResult *result,char *err,size_t len){
 std::memset(result,0,sizeof *result);
 if(!s||s->n<4||s->n>64||!std::isfinite(s->dx)||s->dx<=0||!std::isfinite(s->radius)||s->radius<=0||
    !std::isfinite(s->mu_r)||s->mu_r<=0){std::snprintf(err,len,"invalid 3D magnet grid or material");return false;}
 double L=s->n*s->dx;
 for(int d=0;d<3;d++)if(!std::isfinite(s->center[d])||s->center[d]-s->radius<=0||s->center[d]+s->radius>=L||
  !std::isfinite(s->remanence[d])||!std::isfinite(s->applied[d])){std::snprintf(err,len,"magnet must lie inside the domain with finite fields");return false;}
 try {
  Mesh mesh=Mesh::MakeCartesian3D(s->n,s->n,s->n,Element::HEXAHEDRON,L,L,L);
  H1_FECollection fec(1,3);FiniteElementSpace space(&mesh,&fec);
  auto inside=[&](const Vector &x){double r2=0;for(int d=0;d<3;d++)r2+=(x[d]-s->center[d])*(x[d]-s->center[d]);return r2<s->radius*s->radius;};
  FunctionCoefficient permeability([&](const Vector &x)->real_t{return inside(x)?s->mu_r:1.;});
  VectorFunctionCoefficient remanence(3,[&](const Vector &x,Vector &v){v.SetSize(3);for(int d=0;d<3;d++)v[d]=inside(x)?s->remanence[d]:0;});
  FunctionCoefficient boundary([&](const Vector &x)->real_t{
   double potential=0,r2=0,dot=0;for(int d=0;d<3;d++){
    double X=x[d]-s->center[d];potential-=s->applied[d]*X;r2+=X*X;
    dot+=((s->mu_r-1)*s->applied[d]+s->remanence[d])*X/(s->mu_r+2);
   }
   if(s->exact_sphere_boundary)potential+=s->radius*s->radius*s->radius*dot/std::pow(r2,1.5);
   return potential;
  });
  Array<int> boundary_marker(mesh.bdr_attributes.Max());boundary_marker=1;Array<int> essential;
  space.GetEssentialTrueDofs(boundary_marker,essential);
  GridFunction potential(&space);potential=0.;potential.ProjectBdrCoefficient(boundary,boundary_marker);
  LinearForm rhs(&space);rhs.AddDomainIntegrator(new DomainLFGradIntegrator(remanence));rhs.Assemble();
  BilinearForm form(&space);form.AddDomainIntegrator(new DiffusionIntegrator(permeability));form.Assemble();
  OperatorPtr A;Vector X,B;form.FormLinearSystem(essential,potential,rhs,A,X,B);
  GSSmoother preconditioner(static_cast<SparseMatrix&>(*A));CGSolver cg;
  cg.SetRelTol(1e-12);cg.SetAbsTol(1e-15);cg.SetMaxIter(2000);cg.SetPrintLevel(0);
  cg.SetPreconditioner(preconditioner);cg.SetOperator(*A);cg.Mult(B,X);
  if(!cg.GetConverged()){std::snprintf(err,len,"3D magnetic solve did not converge");return false;}
  form.RecoverFEMSolution(X,rhs,potential);result->iterations=cg.GetNumIterations();
  Vector residual(B.Size());A->Mult(X,residual);residual-=B;
  result->relative_residual=residual.Norml2()/std::fmax(B.Norml2(),1e-30);
  size_t count=(size_t)s->n*s->n*s->n;result->B=(double*)std::calloc(count*3,sizeof(double));
  if(!result->B){std::snprintf(err,len,"cannot allocate 3D magnetic output");return false;}
  IntegrationPoint ip;ip.Set3(.5,.5,.5);Vector grad(3),position(3),br(3);
  for(int e=0;e<mesh.GetNE();e++){
   ElementTransformation *tr=mesh.GetElementTransformation(e);tr->SetIntPoint(&ip);tr->Transform(ip,position);potential.GetGradient(*tr,grad);
   size_t q=(size_t)(position[0]/s->dx)+s->n*((size_t)(position[1]/s->dx)+s->n*(size_t)(position[2]/s->dx));
   if(q>=count){magnet3d_free(result);std::snprintf(err,len,"invalid magnetic sampling index");return false;}
   bool solid=inside(position);double mu=solid?s->mu_r:1;
   for(int d=0;d<3;d++)result->B[3*q+d]=-mu*grad[d]+(solid?s->remanence[d]:0);
  }
  return true;
 }catch(const std::exception &e){magnet3d_free(result);std::snprintf(err,len,"MFEM: %s",e.what());return false;}
}
