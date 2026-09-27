#!/usr/bin/env python3
"""Original demonstration sailplane, metres, +X flow, Y up, Z span.
Closed overlapping components, as with the built-in glider; not a boolean union,
certified airframe or reference aerodynamic geometry. Standard library only.
"""
import math
import struct
from collections import Counter
from pathlib import Path

triangles = []
components = []

def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def loft(rings):
    tris=[]
    n=len(rings[0])
    for a,b in zip(rings,rings[1:]):
        for i in range(n):
            j=(i+1)%n
            tris.extend([(a[i],b[i],b[j]),(a[i],b[j],a[j])])
    for ring,reverse in ((rings[0],False),(rings[-1],True)):
        c=tuple(sum(p[k] for p in ring)/n for k in range(3))
        for i in range(n):
            t=(c,ring[i],ring[(i+1)%n])
            tris.append(t[::-1] if reverse else t)
    volume=sum(dot(a,cross(b,c))/6 for a,b,c in tris)
    if volume<0: tris=[(a,c,b) for a,b,c in tris]
    # Each generated shell must be closed, consistently wound and nondegenerate.
    edges=Counter()
    directed=Counter()
    for a,b,c in tris:
        assert dot(cross(sub(b,a),sub(c,a)),cross(sub(b,a),sub(c,a)))>1e-24
        for u,v in ((a,b),(b,c),(c,a)):
            edges[tuple(sorted((u,v)))]+=1
            directed[(u,v)]+=1
    assert all(v==2 for v in edges.values())
    assert all(directed[(v,u)]==count for (u,v),count in directed.items())
    assert abs(volume)>1e-10
    triangles.extend(tris)
    components.append((len(tris),abs(volume)))

# Smooth fuselage and integrated raised cockpit; tiny closed end rings avoid pole slivers.
rings=[]
for i in range(97):
    t=0.0001+0.9998*i/96
    x=.65*t
    radius=.033*math.sin(math.pi*t)**.65*(1-.72*t)
    cockpit=.025*math.exp(-((t-.23)/.14)**4)
    rings.append([(x, radius*1.1*math.sin(2*math.pi*j/64)+cockpit*max(0,math.sin(2*math.pi*j/64))**2,
                   radius*math.cos(2*math.pi*j/64)) for j in range(64)])
loft(rings)

def section(chord,thickness=.12):
    # Closed symmetric four-digit NACA thickness distribution, cosine spacing.
    loop=[]
    for side,indices in ((1,range(49)),(-1,range(47,0,-1))):
        for i in indices:
            x=(1-math.cos(math.pi*i/48))/2
            y=5*thickness*(.2969*math.sqrt(x)-.126*x-.3516*x*x+.2843*x**3-.1036*x**4)
            camber=.018*math.sin(math.pi*x)
            loop.append((chord*x,chord*(camber+side*y)))
    return loop

# Swept, tapered wings with a gently rising outer winglet; root buried in fuselage.
for sign in (-1,1):
    rings=[]
    for i in range(49):
        t=i/48
        span=.012+.588*t
        chord=.145*(1-.72*t)
        if t>.94: chord*=1-.65*((t-.94)/.06)**2
        rise=.035*t+.075*max(0,(t-.86)/.14)**2
        rings.append([(.17+.07*t+px,.015+rise+py,sign*span) for px,py in section(chord)])
    loft(rings)
# Swept vertical fin and the T-tail.
rings=[]
for i in range(25):
    t=i/24
    rings.append([(.50+.075*t+px,.006+.15*t,py) for px,py in section(.115-.065*t,.10)])
loft(rings)
for sign in (-1,1):
    rings=[]
    for i in range(25):
        t=i/24
        rings.append([(.564+.035*t+px,.151+.006*t+py,sign*(.003+.157*t)) for px,py in section(.075-.050*t,.10)])
    loft(rings)

out=Path(__file__).resolve().parents[1]/'models/showcase-sailplane.stl'
with out.open('wb') as f:
    f.write(b'OpenPhysicsAI original showcase sailplane; metres; closed overlapping shells'.ljust(80,b' '))
    f.write(struct.pack('<I',len(triangles)))
    for a,b,c in triangles:
        n=cross(sub(b,a),sub(c,a));length=math.sqrt(dot(n,n));n=tuple(v/length for v in n)
        f.write(struct.pack('<12fH',*n,*a,*b,*c,0))
print(f'{out.name}: {len(triangles)} triangles, {len(components)} closed outward shells, all edge checks passed')
