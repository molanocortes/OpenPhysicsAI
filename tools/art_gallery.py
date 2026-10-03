#!/usr/bin/env python3
"""Reproducible designed gallery geometries, millimetres; no simulated fields.

Three analytic, triangulated, closed material boundaries. Run with Python's
standard library only: python3 tools/art_gallery.py --output-dir /tmp/art-gallery.
Generated STL files and results are local artifacts, not repository inputs. Validation checks each undirected edge occurs twice,
each directed edge cancels, finite/nondegenerate triangles, positive signed
volume, a single connected surface, and centre-sampled voxel connectivity.
"""
import argparse
import collections
import json
import math
import pathlib
import struct

TAU = 2.0 * math.pi


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def sub(a, b):
    return tuple(x-y for x,y in zip(a,b))


def dot(a, b):
    return sum(x*y for x,y in zip(a,b))


class Mesh:
    def __init__(self, name):
        self.name = name
        self.vertices = []
        self.faces = []
        self.lookup = {}

    def v(self, xyz):
        key = tuple(round(x, 8) for x in xyz)
        if key not in self.lookup:
            self.lookup[key] = len(self.vertices)
            self.vertices.append(tuple(xyz))
        return self.lookup[key]

    def tri(self, a, b, c, reverse=False):
        self.faces.append((a,c,b) if reverse else (a,b,c))

    def quad(self, a, b, c, d, reverse=False):
        self.tri(a,b,c,reverse)
        self.tri(a,c,d,reverse)

    def validate(self):
        edges = collections.Counter()
        oriented = collections.Counter()
        volume = 0.0
        min_area = float('inf')
        for face in self.faces:
            a,b,c = (self.vertices[q] for q in face)
            assert all(math.isfinite(x) for p in (a,b,c) for x in p)
            n = cross(sub(b,a), sub(c,a))
            area = math.sqrt(dot(n,n))/2
            assert area > 1e-12, (self.name, 'degenerate triangle', face)
            min_area = min(min_area, area)
            volume += dot(a, cross(b,c))/6
            for u,v in zip(face,face[1:]+face[:1]):
                edges[min(u,v),max(u,v)] += 1
                oriented[min(u,v),max(u,v)] += 1 if u<v else -1
        bad = [edge for edge,count in edges.items() if count != 2]
        assert not bad, (self.name,'nonmanifold edges',len(bad),bad[:4])
        assert all(value==0 for value in oriented.values()), 'inconsistent winding'
        assert volume > 0, (self.name, 'non-positive volume', volume)
        adjacency = collections.defaultdict(set)
        for u,v in edges:
            adjacency[u].add(v)
            adjacency[v].add(u)
        reached = {0}
        queue = collections.deque([0])
        while queue:
            for v in adjacency[queue.popleft()]:
                if v not in reached:
                    reached.add(v)
                    queue.append(v)
        assert len(reached)==len(self.vertices), 'disconnected surface'
        bounds = [[min(v[axis] for v in self.vertices),max(v[axis] for v in self.vertices)] for axis in range(3)]
        return dict(vertices=len(self.vertices),triangles=len(self.faces),edges=len(edges),
                    nonmanifold_edges=0,inconsistent_edges=0,surface_components=1,
                    euler_characteristic=len(self.vertices)-len(edges)+len(self.faces),
                    volume_mm3=volume,min_triangle_area_mm2=min_area,bounds_mm=bounds)

    def write_stl(self, output_dir):
        path = output_dir / (self.name+'.stl')
        with path.open('wb') as f:
            f.write(('OpenPhysicsAI designed geometry; units mm; '+self.name).encode()[:80].ljust(80,b' '))
            f.write(struct.pack('<I',len(self.faces)))
            for face in self.faces:
                a,b,c = (self.vertices[q] for q in face)
                n = cross(sub(b,a),sub(c,a))
                size = math.sqrt(dot(n,n))
                f.write(struct.pack('<12fH',*(x/size for x in n),*a,*b,*c,0))
        # Re-read the stored float32 coordinates, so watertightness is also
        # established for the actual delivered artifact.
        data=path.read_bytes()
        edges=collections.Counter()
        min_area=float('inf')
        for i in range(len(self.faces)):
            row=struct.unpack_from('<12fH',data,84+50*i)
            points=[tuple(row[j:j+3]) for j in (3,6,9)]
            nn=cross(sub(points[1],points[0]),sub(points[2],points[0]))
            area=math.sqrt(dot(nn,nn))/2
            assert area>1e-12, 'degenerate triangle after float32 serialization'
            min_area=min(min_area,area)
            for a,b in zip(points,points[1:]+points[:1]):
                edges[tuple(sorted((a,b)))]+=1
        assert all(n==2 for n in edges.values()), 'STL serialization changed topology'
        self.serialized_validation=dict(nonmanifold_edges=0,degenerate_triangles=0,
                                        min_triangle_area_mm2=min_area)
        return str(path)


def vase_radius(theta,z):
    t = z/24.0
    return 6.7 + 1.5*math.sin(math.pi*t) + .45*t + .60*math.cos(7*(theta-1.75*t))


def make_vase():
    m = Mesh('helical_flute_vase_mm')
    n = 192
    outer_z = [k*24/96 for k in range(97)]
    inner_z = [1.5+k*(24-1.5)/90 for k in range(91)]
    rings = []
    for zs,offset in ((outer_z,0),(inner_z,2.1)):
        rr = []
        for z in zs:
            rr.append([m.v(((vase_radius(TAU*j/n,z)-offset)*math.cos(TAU*j/n),
                            (vase_radius(TAU*j/n,z)-offset)*math.sin(TAU*j/n),z)) for j in range(n)])
        for k in range(len(rr)-1):
            for j in range(n):
                q=(j+1)%n
                m.quad(rr[k][j],rr[k][q],rr[k+1][q],rr[k+1][j],reverse=bool(offset))
        rings.append(rr)
    outside,inside = rings
    low=m.v((0,0,0))
    floor=m.v((0,0,1.5))
    for j in range(n):
        q=(j+1)%n
        m.tri(low,outside[0][q],outside[0][j])
        m.tri(floor,inside[0][j],inside[0][q])
        m.quad(outside[-1][j],outside[-1][q],inside[-1][q],inside[-1][j])
    return m


COL_HEIGHT=24.0
COL_THICKNESS=1.8
COL_TWIST=1.40
COL_NTHETA=8
COL_NZ=3
COL_START=2.25
COL_END=21.75
HOLE_A=.235 # angular halfwidth, radians
HOLE_B=2.20 # axial halfheight, mm
BEVEL=.18  # radius of hole-edge quarter round, mm


def column_radius(z):
    return 7.70+.45*math.cos(TAU*z/COL_HEIGHT)


def column_xyz(u,z,depth=0):
    theta=u+COL_TWIST*z/COL_HEIGHT
    r=column_radius(z)-depth
    return (r*math.cos(theta),r*math.sin(theta),z)


def make_column():
    m=Mesh('helical_window_column_mm')
    segments=64
    bands=5
    tileu=TAU/COL_NTHETA
    tilez=(COL_END-COL_START)/COL_NZ
    # Expanded opening at both outer faces; a small quarter round joins each
    # face to the straight through-wall window. This is designed geometry.
    depths = [0,.18*(1-math.cos(math.pi/8)),.18*(1-math.cos(math.pi/4)),
              .18*(1-math.cos(3*math.pi/8)),.18,
              COL_THICKNESS-.18,
              COL_THICKNESS-.18*(1-math.cos(3*math.pi/8)),
              COL_THICKNESS-.18*(1-math.cos(math.pi/4)),
              COL_THICKNESS-.18*(1-math.cos(math.pi/8)),COL_THICKNESS]

    def expansion(depth):
        d=min(depth,COL_THICKNESS-depth)
        if d>=BEVEL: return 0.0
        return BEVEL-math.sqrt(max(0.0,BEVEL*BEVEL-(BEVEL-d)**2))

    def hole(uc,zc,phi,depth):
        e=expansion(depth)
        return uc+(HOLE_A+e/column_radius(zc))*math.cos(phi), zc+(HOLE_B+e)*math.sin(phi)

    for row in range(COL_NZ):
        zc=COL_START+(row+.5)*tilez
        for col in range(COL_NTHETA):
            uc=(col+.5)*tileu
            walls=[]
            for depth in (0,COL_THICKNESS):
                rings=[]
                for band in range(bands+1):
                    t=band/bands
                    ring=[]
                    for j in range(segments):
                        phi=TAU*j/segments
                        cp,sp=math.cos(phi),math.sin(phi)
                        norm=max(abs(cp),abs(sp))
                        uo=uc+.5*tileu*cp/norm
                        zo=zc+.5*tilez*sp/norm
                        ui,zi=hole(uc,zc,phi,depth)
                        ring.append(m.v(column_xyz(ui+(uo-ui)*t,zi+(zo-zi)*t,depth)))
                    rings.append(ring)
                for band in range(bands):
                    for j in range(segments):
                        q=(j+1)%segments
                        m.quad(rings[band][j],rings[band+1][j],rings[band+1][q],rings[band][q],reverse=bool(depth))
                walls.append(rings[0])
            wallrings=[]
            for depth in depths:
                wallrings.append([m.v(column_xyz(*hole(uc,zc,TAU*j/segments,depth),depth)) for j in range(segments)])
            for k in range(len(wallrings)-1):
                for j in range(segments):
                    q=(j+1)%segments
                    m.quad(wallrings[k][j],wallrings[k][q],wallrings[k+1][q],wallrings[k+1][j])

    # Exactly reuse the angular subdivision on the window-patch boundary.
    us=[]
    for col in range(COL_NTHETA):
        uc=(col+.5)*tileu
        for j in range(segments):
            phi=TAU*j/segments
            cp,sp=math.cos(phi),math.sin(phi)
            if abs(sp)>=abs(cp)-1e-12:
                us.append((uc+.5*tileu*cp/max(abs(cp),abs(sp)))%TAU)
    us=sorted(set(0.0 if abs(u-TAU)<1e-8 or abs(u)<1e-8 else round(u,12) for u in us))
    # Rounding u above is below mesh welding precision; all seams share ids.
    outer_bottom=inner_floor=outer_top=inner_top=None
    for depth,zlevels in ((0,[0,.75,1.5,COL_START]),
                          (COL_THICKNESS,[1.5,COL_START]),
                          (0,[COL_END,22.5,23.25,24]),
                          (COL_THICKNESS,[COL_END,22.5,23.25,24])):
        rr=[[m.v(column_xyz(u,z,depth)) for u in us] for z in zlevels]
        for k in range(len(rr)-1):
            for j in range(len(us)):
                q=(j+1)%len(us)
                m.quad(rr[k][j],rr[k][q],rr[k+1][q],rr[k+1][j],reverse=bool(depth))
        if depth==0 and zlevels[0]==0: outer_bottom=rr[0]
        if depth>0 and zlevels[0]==1.5: inner_floor=rr[0]
        if depth==0 and zlevels[-1]==24: outer_top=rr[-1]
        if depth>0 and zlevels[-1]==24: inner_top=rr[-1]
    low=m.v((0,0,0)); floor=m.v((0,0,1.5))
    for j in range(len(us)):
        q=(j+1)%len(us)
        m.tri(low,outer_bottom[q],outer_bottom[j])
        m.tri(floor,inner_floor[j],inner_floor[q])
        m.quad(outer_top[j],outer_top[q],inner_top[q],inner_top[j])
    return m


def inside_vase(x,y,z):
    if not 0<=z<=24: return False
    theta=math.atan2(y,x)
    r=math.hypot(x,y)
    ro=vase_radius(theta,z)
    return r<=ro and (z<=1.5 or r>=ro-2.1)


def inside_column(x,y,z):
    if not 0<=z<=24: return False
    r=math.hypot(x,y)
    ro=column_radius(z)
    if r>ro: return False
    if z<=1.5: return True
    if r<ro-COL_THICKNESS: return False
    if not COL_START<z<COL_END: return True
    u=(math.atan2(y,x)-COL_TWIST*z/COL_HEIGHT)%TAU
    tileu=TAU/COL_NTHETA
    tilez=(COL_END-COL_START)/COL_NZ
    uc=(int(u/tileu)+.5)*tileu
    row=min(COL_NZ-1,int((z-COL_START)/tilez))
    zc=COL_START+(row+.5)*tilez
    depth=ro-r
    d=min(depth,COL_THICKNESS-depth)
    extra=BEVEL-math.sqrt(max(0,BEVEL**2-(BEVEL-d)**2)) if d<BEVEL else 0
    return ((u-uc)/(HOLE_A+extra/column_radius(zc)))**2+((z-zc)/(HOLE_B+extra))**2 >=1


def voxel_check(bounds,inside,h):
    # Estimates only; the native mesher independently classifies actual STL.
    origin=[bounds[i][0] for i in range(3)]
    counts=[math.ceil((bounds[i][1]-origin[i])/h) for i in range(3)]
    solid=set()
    for k in range(counts[2]):
        z=origin[2]+(k+.5)*h
        for j in range(counts[1]):
            y=origin[1]+(j+.5)*h
            for i in range(counts[0]):
                x=origin[0]+(i+.5)*h
                if inside(x,y,z): solid.add((i,j,k))
    base={p for p in solid if p[2]==0}
    reached=set(base); q=collections.deque(base)
    while q:
        i,j,k=q.popleft()
        for p in ((i-1,j,k),(i+1,j,k),(i,j-1,k),(i,j+1,k),(i,j,k-1),(i,j,k+1)):
            if p in solid and p not in reached:
                reached.add(p);q.append(p)
    # Every new layer should have at least one face-connected link to the
    # preceding layer within each of its in-plane components.
    floating_layers=[]
    for k in range(1,counts[2]):
        pending={p for p in solid if p[2]==k}
        while pending:
            first=pending.pop(); component={first}; q=collections.deque([first])
            while q:
                i,j,z=q.popleft()
                for p in ((i-1,j,z),(i+1,j,z),(i,j-1,z),(i,j+1,z)):
                    if p in pending:
                        pending.remove(p);component.add(p);q.append(p)
            if not any((i,j,k-1) in solid for i,j,_ in component):
                floating_layers.append(k)
    return dict(h_mm=h,estimated_active_elements=len(solid),base_elements=len(base),
                elements_disconnected_from_base=len(solid-reached),
                layers_with_unsupported_components=sorted(set(floating_layers)),
                method='analytic centre samples; native STL mesher must confirm')



COROLLA_HUB=3.5
COROLLA_WIDTH=1.65
COROLLA_TIP_CENTRE=12.65
COROLLA_HEIGHT=18.0
COROLLA_TWIST=.35
COROLLA_FINS=12


def corolla_centre(r):
    t=max(0.0,(r-3.3)/(COROLLA_TIP_CENTRE-3.3))
    phi=.30*t*t
    derivative=.60*t/(COROLLA_TIP_CENTRE-3.3)
    c=(r*math.cos(phi),r*math.sin(phi))
    tangent=(math.cos(phi)-r*derivative*math.sin(phi),
             math.sin(phi)+r*derivative*math.cos(phi))
    length=math.hypot(*tangent)
    tangent=tuple(v/length for v in tangent)
    return c,tangent


def corolla_side(r,sign):
    c,tangent=corolla_centre(r)
    return (c[0]-sign*COROLLA_WIDTH/2*tangent[1],
            c[1]+sign*COROLLA_WIDTH/2*tangent[0])


def rotate_xy(p,angle):
    c,s=math.cos(angle),math.sin(angle)
    return (p[0]*c-p[1]*s,p[0]*s+p[1]*c)


def corolla_outline():
    roots=[]
    for sign in (-1,1):
        lo,hi=3.0,4.0
        for _ in range(50):
            mid=(lo+hi)/2
            if math.hypot(*corolla_side(mid,sign))>COROLLA_HUB: hi=mid
            else: lo=mid
        roots.append((lo+hi)/2)
    one=[]
    radial_steps=20
    cap_steps=16
    for j in range(radial_steps+1):
        r=roots[0]+(COROLLA_TIP_CENTRE-roots[0])*j/radial_steps
        one.append(corolla_side(r,-1))
    centre,tangent=corolla_centre(COROLLA_TIP_CENTRE)
    lam=math.atan2(tangent[1],tangent[0])
    tip_index=len(one)-1+cap_steps//2
    for j in range(1,cap_steps+1):
        angle=lam-math.pi/2+math.pi*j/cap_steps
        one.append((centre[0]+COROLLA_WIDTH/2*math.cos(angle),centre[1]+COROLLA_WIDTH/2*math.sin(angle)))
    for j in range(1,radial_steps+1):
        r=COROLLA_TIP_CENTRE+(roots[1]-COROLLA_TIP_CENTRE)*j/radial_steps
        one.append(corolla_side(r,1))
    a=math.atan2(one[-1][1],one[-1][0])
    b=math.atan2(one[0][1],one[0][0])+TAU/COROLLA_FINS
    for j in range(1,4):
        angle=a+(b-a)*j/4
        one.append((COROLLA_HUB*math.cos(angle),COROLLA_HUB*math.sin(angle)))
    outline=[]
    tips=[]
    for fin in range(COROLLA_FINS):
        tips.append(len(outline)+tip_index)
        outline.extend(rotate_xy(p,TAU*fin/COROLLA_FINS) for p in one)
    return outline,tips


def triangulate_polygon(points):
    """Ear clipping for a simple counter-clockwise analytic polygon."""
    def turn(a,b,c):
        return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
    remaining=list(range(len(points)))
    triangles=[]
    assert sum(points[i][0]*points[(i+1)%len(points)][1]-points[(i+1)%len(points)][0]*points[i][1] for i in range(len(points)))>0
    while len(remaining)>3:
        clipped=False
        for at,b in enumerate(remaining):
            a=remaining[at-1]; c=remaining[(at+1)%len(remaining)]
            pa,pb,pc=points[a],points[b],points[c]
            if turn(pa,pb,pc)<=1e-11: continue
            lo=(min(pa[0],pb[0],pc[0]),min(pa[1],pb[1],pc[1]))
            hi=(max(pa[0],pb[0],pc[0]),max(pa[1],pb[1],pc[1]))
            obstructed=False
            for q in remaining:
                if q in (a,b,c): continue
                p=points[q]
                if not (lo[0]-1e-10<=p[0]<=hi[0]+1e-10 and lo[1]-1e-10<=p[1]<=hi[1]+1e-10): continue
                if min(turn(pa,pb,p),turn(pb,pc,p),turn(pc,pa,p))>=-1e-10:
                    obstructed=True;break
            if obstructed: continue
            triangles.append((a,b,c));remaining.pop(at);clipped=True;break
        assert clipped, 'polygon could not be triangulated'
    triangles.append(tuple(remaining))
    return triangles


def make_corolla():
    m=Mesh('thermal_corolla_mm')
    outline,tips=corolla_outline()
    rings=[]
    for k in range(35):
        t=k/34
        z=1.5+(COROLLA_HEIGHT-1.5)*t
        rings.append([m.v((*rotate_xy(p,COROLLA_TWIST*t),z)) for p in outline])
    for k in range(len(rings)-1):
        for j in range(len(outline)):
            q=(j+1)%len(outline)
            m.quad(rings[k][j],rings[k][q],rings[k+1][q],rings[k+1][j])
    for a,b,c in triangulate_polygon(outline):
        m.tri(rings[-1][a],rings[-1][b],rings[-1][c])
    angle0=math.atan2(outline[tips[0]][1],outline[tips[0]][0])
    outer_n=COROLLA_FINS*16
    outerxy=[(14*math.cos(angle0+TAU*j/outer_n),14*math.sin(angle0+TAU*j/outer_n)) for j in range(outer_n)]
    low=[m.v((*p,0)) for p in outerxy]
    high=[m.v((*p,1.5)) for p in outerxy]
    centre=m.v((0,0,0))
    for j in range(outer_n):
        q=(j+1)%outer_n
        m.tri(centre,low[q],low[j])
        m.quad(low[j],low[q],high[q],high[j])
    # Each exposed base sector is one simple polygon between two fin tips.
    for fin in range(COROLLA_FINS):
        current=tips[fin]
        following=tips[(fin+1)%COROLLA_FINS]
        if following<current: following+=len(outline)
        ii=[q%len(outline) for q in range(following,current-1,-1)]
        outer_ii=[q%outer_n for q in range(fin*16,(fin+1)*16+1)]
        xy=[outline[q] for q in ii]+[outerxy[q] for q in outer_ii]
        vv=[rings[0][q] for q in ii]+[high[q] for q in outer_ii]
        for a,b,c in triangulate_polygon(xy):
            m.tri(vv[a],vv[b],vv[c])
    return m


COROLLA_OUTLINE=None

def inside_corolla(x,y,z):
    if not 0<=z<=COROLLA_HEIGHT: return False
    r=math.hypot(x,y)
    if z<=1.5: return r<=14
    if r<=COROLLA_HUB: return True
    if r>13.48: return False
    global COROLLA_OUTLINE
    if COROLLA_OUTLINE is None: COROLLA_OUTLINE=corolla_outline()[0]
    x,y=rotate_xy((x,y),-COROLLA_TWIST*(z-1.5)/(COROLLA_HEIGHT-1.5))
    # Centre sample of the actual polygon, no analytic-curve approximation.
    hit=False
    for a,b in zip(COROLLA_OUTLINE,COROLLA_OUTLINE[1:]+COROLLA_OUTLINE[:1]):
        if (a[1]>y)!=(b[1]>y) and x<(b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0]: hit=not hit
    return hit

def generate(output_dir):
    """Write designed STL inputs and validation manifest into output_dir."""
    output_dir=pathlib.Path(output_dir).expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    result={}
    for mesh,inside,title,detail in (
        (make_vase(),inside_vase,'Helical Flute',
         dict(type='hollow fluted vase',height_mm=24,radial_wall_mm=2.1,base_mm=1.5,flutes=7,twist_degrees=1.75*180/math.pi)),
        (make_column(),inside_column,'Helical Lantern',
         dict(type='porous shell with 24 rounded helical windows',height_mm=24,radial_wall_mm=1.8,base_mm=1.5,
              windows=24,window_height_mm=4.4,window_angular_width_radians=.47,hole_edge_round_mm=.18,
              twist_degrees=COL_TWIST*180/math.pi)),
        (make_corolla(),inside_corolla,'Thermal Corolla',
         dict(type='united heat-sink with 12 swept, twisted radial fins',height_mm=COROLLA_HEIGHT,
              base_radius_mm=14,base_mm=1.5,hub_radius_mm=COROLLA_HUB,fins=12,
              fin_width_mm=COROLLA_WIDTH,twist_degrees=COROLLA_TWIST*180/math.pi,
              fin_centreline_sweep_degrees=.30*180/math.pi))):
        stats=mesh.validate()
        path=mesh.write_stl(output_dir)
        stats.update(title=title,design=detail,path=path,units='mm',serialized_stl_validation=mesh.serialized_validation,
                     recommended_camera=dict(azimuth_degrees=35,elevation_degrees=38 if mesh.name=='thermal_corolla_mm' else 26,look_at_mm=[0,0,9 if mesh.name=='thermal_corolla_mm' else 12]),
                     provenance='Analytically designed input geometry, not a simulated or measured result.')
        stats['voxel_estimates']=[voxel_check(stats['bounds_mm'],inside,h) for h in (.75,.8,1.0)]
        result[mesh.name]=stats
    (output_dir/'geometry_manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=pathlib.Path, required=True,
                        help='Directory for regenerated STL inputs and geometry_manifest.json')
    args=parser.parse_args()
    print(json.dumps(generate(args.output_dir), indent=2))


if __name__=='__main__':
    main()
