/* Synthetic rendering-contract tests, not physical validation. Criteria fixed before execution:
 * exact voxel boundary counts, analytical warped coordinates at every stored time, no temperature on metal,
 * displayed-only range/max, crease identity, and a cached rebuild timing on 29^3 = 24389 elements. */
#include "../src/fembridge.c"
/* Input is exercised separately by uicheck; these guards forbid accidental UI use in this pure geometry test. */
int app_inject_click(float x,float y,int n) { abort(); }
int app_inject_drag(float x,float y,float x1,float y1,uint32_t m,int n) { abort(); }
int app_inject_scroll(float x,float y,float dy) { abort(); }
int app_inject_key(int key) { abort(); }
bool app_rake_handle(float *x,float *y) { abort(); }
bool app_export_image(const char *path,int scale) { abort(); } /* EXPORT IMAGE belongs to the window, not to this test */
static int passed, failed;
#define CHECK(x, msg) do { if (x) passed++; else { failed++; printf("FAIL: %s\n", msg); } } while (0)
static int node(int x,int y,int z,int n) { return x+(n+1)*(y+(n+1)*z); }
static ThermalCase *fixture(int n) {
    ThermalCase *c=calloc(1,sizeof *c);
    c->nnodes=(n+1)*(n+1)*(n+1); c->nelems=n*n*n; c->noutputs=3; c->has_mech=true;
    c->xyz=calloc(3*(size_t)c->nnodes,sizeof(double)); c->conn=calloc(8*(size_t)c->nelems,sizeof(int));
    c->times=calloc(3,sizeof(double)); c->mech_u=calloc(9*(size_t)c->nnodes,sizeof(double));
    c->mech_vm=calloc(3*(size_t)c->nnodes,sizeof(double));
    for(int z=0;z<=n;z++) for(int y=0;y<=n;y++) for(int x=0;x<=n;x++) {
        int k=node(x,y,z,n); c->xyz[3*k]=x*.001; c->xyz[3*k+1]=y*.001; c->xyz[3*k+2]=z*.001;
        for(int t=0;t<3;t++) {
            c->times[t]=t;
            c->mech_u[3*((size_t)t*c->nnodes+k)]=(t+1)*z*1e-5;
            c->mech_vm[(size_t)t*c->nnodes+k]=(t+1)*z*1e6;
        }
    }
    for(int z=0;z<n;z++) for(int y=0;y<n;y++) for(int x=0;x<n;x++) {
        int e=x+n*(y+n*z);
        int q[]={node(x,y,z,n),node(x+1,y,z,n),node(x+1,y+1,z,n),node(x,y+1,z,n),
                 node(x,y,z+1,n),node(x+1,y,z+1,n),node(x+1,y+1,z+1,n),node(x,y+1,z+1,n)};
        memcpy(c->conn+8*e,q,sizeof q);
    }
    return c;
}
static void attach(ThermalCase *c,const char *id,const char *kind) {
    jobs_attach(app.engine->jobs,id,kind,"/tmp",c,thermal_case_free,NULL);
    jobs_release(app.engine->jobs,id); fem_show_job(id); rebuild_surface();
}
static void coordinates(ThermalCase *c) {
    SurfSrc s={0}; int ns; bool mech; double time;
    field_values(c,"lpbf_build",B.field,B.step,&s,&time,&ns,&mech);
    result_visibility(c,"lpbf_build",c->nelems,&s.vis); s.adj=B.adj; s.step=B.step;
    size_t v=0; bool ok=true;
    for(int e=0;e<c->nelems;e++) if(element_shown(&s,e)) for(int f=0;f<6;f++) {
        int nb=s.adj[6*e+f]; if(nb>=0 && element_shown(&s,nb)) continue;
        for(int k=0;k<4;k++,v++) {
            int n=c->conn[8*e+HEX8_FACE_NODES[f][k]];
            const double *x=c->xyz+3*n,*u=c->mech_u+3*((size_t)B.step*c->nnodes+n);
            double expect[]={B.world_centre.x+B.to_world_scale*(x[0]+u[0]*B.deform_used-B.centre_m[0]),
                B.world_centre.y+B.to_world_scale*(x[2]+u[2]*B.deform_used-B.centre_m[2]),
                B.world_centre.z-B.to_world_scale*(x[1]+u[1]*B.deform_used-B.centre_m[1])};
            for(int a=0;a<3;a++) if(fabs(B.surf.verts[7*v+a]-expect[a])>2e-5) ok=false;
        }
    }
    CHECK(ok && v==B.surf.nverts,"every visible vertex uses the shown time and scale (no origin/undeformed flash)");
    free(s.value);
}
/* Deliberately sparse STL: one pair of facets spans every face of the entire cube. A cut through its middle
 * leaves all corner elements alive, so corner-only visibility tests would incorrectly bridge the gap. */
static Body *sparse_cube(int n) {
    Body *b=calloc(1,sizeof *b);
    snprintf(b->name,sizeof b->name,"sparse-cube");
    b->surf.nv=8; b->surf.nt=12;
    b->build_v=calloc(24,sizeof(double)); b->build_normal=calloc(36,sizeof(double));
    b->surf.tri=calloc(36,sizeof(int));
    const int corners[8][3]={{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}};
    const int quads[6][4]={{0,3,2,1},{4,5,6,7},{0,1,5,4},{2,3,7,6},{1,2,6,5},{0,4,7,3}};
    const double normals[6][3]={{0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{1,0,0},{-1,0,0}};
    for(int v=0;v<8;v++) for(int a=0;a<3;a++) b->build_v[3*v+a]=corners[v][a]*n*.001;
    for(int f=0;f<6;f++) {
        int tris[6]={quads[f][0],quads[f][1],quads[f][2],quads[f][0],quads[f][2],quads[f][3]};
        memcpy(b->surf.tri+6*f,tris,sizeof tris);
        for(int t=0;t<2;t++) memcpy(b->build_normal+3*(2*f+t),normals[f],sizeof normals[f]);
    }
    return b;
}
static void sparse_surface_checks(void) {
    fem_section(-1,.5); fem_show_group(0,true); fem_show_group(1,true); fem_show_group(2,true);
    Project *p=project_new("sparse-cube","/tmp/navier-femviewtest/sparse-cube", "Synthetic rendering contract");
    project_add_body(p,sparse_cube(6));
    engine_lock(app.engine); engine_set_project(app.engine,p); engine_unlock(app.engine);
    ThermalCase *c=fixture(6); fem_set_surface_view(true); attach(c,"view-sparse","lpbf_build");
    CHECK(B.st.on_surface && B.st.surface_tris==12,"intact sparse STL stays smooth with twelve mapped facets");
    c->elem_death=malloc((size_t)c->nelems*sizeof(int));
    for(int e=0;e<c->nelems;e++) c->elem_death[e]=e/36==2 ? 2 : -1;
    fem_set_step(2); rebuild_surface();
    CHECK(!B.st.on_surface && B.st.surface_tris==528,"sparse STL cannot bridge a removed interior element layer");
    free(c->elem_death); c->elem_death=NULL;
    c->elem_birth=malloc((size_t)c->nelems*sizeof(int));
    for(int e=0;e<c->nelems;e++) c->elem_birth[e]=(e/36)/2;
    fem_set_step(0); rebuild_surface();
    CHECK(!B.st.on_surface && B.st.surface_tris==240,"sparse STL growth draws exactly the first two layers and their top");
    fem_set_step(2); rebuild_surface();
    CHECK(B.st.on_surface && B.st.surface_tris==12,"fully born intact geometry returns to the mapped STL");
    fem_section(0,.5); rebuild_surface();
    CHECK(!B.st.on_surface && B.st.surface_tris==288,"sparse STL section uses a closed active mesh boundary");
    fem_section(-1,.5);
    c->elem_group=calloc((size_t)c->nelems,1);
    for(int e=0;e<36;e++) c->elem_group[e]=1;
    fem_set_step(2); rebuild_surface();
    CHECK(!B.st.on_surface && B.st.surface_tris==432,"shown support geometry stays on the FE boundary even when all elements exist");
    for(int e=0;e<36;e++) c->elem_group[e]=2;
    rebuild_surface();
    bool grey=false;
    for(uint32_t v=0;v<B.surf.nverts;v++) if(B.surf.verts[7*(size_t)v+6]<-1e20f) grey=true;
    CHECK(!B.st.on_surface && B.st.surface_tris==432 && grey,"shown plate geometry retains its FE boundary and neutral grey scalar sentinel");
    for(int e=0;e<36;e++) c->elem_group[e]=1;
    fem_show_group(1,false); rebuild_surface();
    CHECK(!B.st.on_surface && B.st.surface_tris==384,"hidden groups also open a closed finite-element boundary");
    fem_show_group(1,true);
    printf("sparse STL: intact 12; cut 528; early growth 240; half section 288; hidden support 384 triangles\n");

    /* A coarse staircase can lie inside the original skin even at zero deformation. FIT at the first layer must
     * include the final mapped source geometry as well as the union of computed mesh nodes. */
    Project *offset=project_new("offset-cube","/tmp/navier-femviewtest/offset-cube", "Non-grid-aligned rendering contract");
    Body *ob=sparse_cube(6);
    for(int v=0;v<8;v++) for(int a=0;a<3;a++) ob->build_v[3*v+a]=ob->build_v[3*v+a]==0 ? -.00025 : .00625;
    project_add_body(offset,ob);
    engine_lock(app.engine); engine_set_project(app.engine,offset); engine_unlock(app.engine);
    ThermalCase *of=fixture(6);
    memset(of->mech_u,0,9*(size_t)of->nnodes*sizeof(double));
    of->elem_birth=calloc((size_t)of->nelems,sizeof(int));
    for(int e=0;e<of->nelems;e++) of->elem_birth[e]=(e/36)/2;
    fem_set_deform_scale(1); attach(of,"view-offset","fff_print");
    fem_set_step(0); rebuild_surface();
    vec3 flo,fhi; bool fits=fem_world_bounds(&flo,&fhi);
    fem_set_step(2); rebuild_surface();
    CHECK(B.st.on_surface && B.st.surface_tris==12,"non-grid-aligned fully born print uses its mapped source STL");
    for(uint32_t v=0;v<B.surf.nverts;v++) {
        const float *q=B.surf.verts+7*(size_t)v;
        if(q[0]<flo.x-2e-5 || q[0]>fhi.x+2e-5 || q[1]<flo.y-2e-5 || q[1]>fhi.y+2e-5 ||
           q[2]<flo.z-2e-5 || q[2]>fhi.z+2e-5) fits=false;
    }
    CHECK(fits,"first-layer FIT also contains the final mapped STL beyond the staircase at zero displacement");
}
int main(void) {
    EngineConfig cfg; engine_config_default(&cfg); char err[256];
    snprintf(cfg.workspace,sizeof cfg.workspace,"/tmp/navier-femviewtest");
    app.engine=engine_create(&cfg,err,sizeof err); if(!app.engine) { puts(err); return 2; }
    app.nx=app.ny=app.nz=64; fem_init();
    CHECK(result_kind_of("fff_print")==RK_TRANSIENT && result_kind_of("lpbf_build")==RK_TRANSIENT,"both print kinds use stored results");
    ThermalCase *c=fixture(6); attach(c,"view-metal","lpbf_build");
    CHECK(fem_field_available(FEM_VON_MISES) && fem_field_available(FEM_DISPLACEMENT) && !fem_field_available(FEM_TEMPERATURE),"metal exposes only carried fields");
    fem_set_field(FEM_TEMPERATURE); rebuild_surface();
    CHECK(B.field==FEM_VON_MISES,"missing temperature safely falls back to stress");
    int whole=B.st.surface_tris;
    CHECK(whole==12*6*6,"whole cube matches analytical boundary triangle count");
    CHECK(B.outline_vertices==24*6,"outline contains only cube crease segments");
    CHECK(fem_fake_cut(2,2.5,2),"cut at zero-based stored index accepted");
    fem_set_step(1); rebuild_surface(); CHECK(B.st.surface_tris==whole,"cut does not happen before death index");
    fem_set_step(2); rebuild_surface();
    /* 2 new 6x6 surfaces, minus the four external 6x1 strips of the removed layer. */
    CHECK(B.st.surface_tris==whole+4*6*6-8*6,"cut exposes both sides and removes the layer's outside faces");
    printf("cut triangles %d -> %d (new surfaces +144, removed perimeter -48)\n",whole,B.st.surface_tris);
    fem_fake_cut_clear(); fem_set_step(0); rebuild_surface(); CHECK(B.st.surface_tris==whole,"null lifetimes preserve identity");
    fem_section(0,.5); rebuild_surface(); CHECK(B.st.surface_tris==288,"half-cube section surface includes interior face");
    fem_section_flip(); rebuild_surface(); CHECK(B.st.surface_tris==288,"flipped half has same boundary count");
    fem_section(-1,.5); fem_section_flip();
    fem_fake_groups(2,2,4); fem_show_group(0,false); fem_show_group(2,false); fem_set_range_all(false); rebuild_surface();
    CHECK(fabs(B.st.range_hi-4)<1e-6 && fabs(B.st.range_lo-2)<1e-6,"range excludes hidden part and plate");
    fem_show_group(2,true); rebuild_surface();
    CHECK(fabs(B.st.range_hi-4)<1e-6 && fabs(B.st.range_lo-2)<1e-6,"neutral plate does not affect range");
    bool neutral=false; for(unsigned i=0;i<B.surf.nverts;i++) if(B.surf.verts[7*i+6]<-2e30f) neutral=true;
    CHECK(neutral,"plate vertices carry neutral colour marker");
    fem_set_range_all(true); rebuild_surface(); CHECK(fabs(B.st.range_hi-12)<1e-6,"all-times range respects selected groups");
    fem_show_group(0,true); fem_show_group(1,true); fem_show_group(2,true);
    fem_fake_growth(2); fem_fake_cut(2,2.5,2);
    for(int mode=0;mode<3;mode++) {
        fem_set_deform_scale(mode==0 ? 1 : mode==1 ? 10 : -1);
        fem_set_step(0); rebuild_surface();
        vec3 fit_lo, fit_hi;
        CHECK(fem_world_bounds(&fit_lo,&fit_hi),"first-layer print FIT produces all-time bounds");
        fem_set_playing(true); app.time=10; B.play_clock=10; B.play_pos=0;
        bool fit_covers=true;
        for(int t=0;t<3;t++) {
            if(t) { app.time+=.25; advance_playback(); }
            rebuild_surface(); CHECK(B.step==t,"playback visits each stored time"); coordinates(c);
            for(uint32_t i=0;i<B.surf.nverts;i++) {
                const float *p=B.surf.verts+7*(size_t)i;
                if(p[0]<fit_lo.x-2e-5 || p[0]>fit_hi.x+2e-5 || p[1]<fit_lo.y-2e-5 || p[1]>fit_hi.y+2e-5 ||
                   p[2]<fit_lo.z-2e-5 || p[2]>fit_hi.z+2e-5) fit_covers=false;
            }
            double expected=(t+1)*(t+1)*.02;
            CHECK(fabs(B.st.max_displacement_mm-expected)<1e-8,"max displacement excludes not-yet-born nodes");
            int count; const float *o=fem_outline(&count); bool in_bounds=o && count>0;
            double maxz=B.world_centre.y+B.to_world_scale*((t+1)*.002-B.centre_m[2]);
            for(int i=0;i<count;i++) if(o[3*i+1]>maxz+2e-5) in_bounds=false;
            CHECK(in_bounds,"undeformed outline follows current birth/death visibility");
        }
        CHECK(fit_covers,"first-layer FIT bounds contain every later visible deformed vertex");
        fem_set_playing(false);
    }
    ThermalCase *big=fixture(29); attach(big,"view-bench","fff_print");
    CHECK(!B.fake_birth && !B.fake_death && !B.fake_group,"debug arrays never leak into a different job");
    fem_section(0,.5); double total=0,worst=0;
    for(int t=0;t<12;t++) { fem_set_step(t%3); rebuild_surface(); total+=B.rebuild_ms; worst=MAXI(worst,B.rebuild_ms); }
    printf("24389 elements, section+AUTO+outline+all-times: mean %.3f ms, max %.3f ms (12 rebuilds)\n",total/12,worst);
    fem_show_group(0,false); rebuild_surface(); CHECK(B.st.have_result && !B.surface_valid,"empty view retains result controls");
    sparse_surface_checks();
    fem_shutdown(); engine_destroy(app.engine); app.engine=NULL;
    printf("FEM VIEW CHECKS: %d passed, %d failed\n",passed,failed); return failed ? 1 : 0;
}
