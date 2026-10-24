/* contact.h - thermal interfaces between bodies of one voxel mesh: topology, diagnostics and the thermal node split
 *
 * Two bodies touch in the mesh where an element of one is the face neighbour of an element of the other. Such faces are
 * interior to the mesh (the mesher lists only faces without a neighbour as boundary faces), and the two bodies share the
 * nodes of those faces. That sharing is perfect thermal and mechanical bonding.
 *
 * A finite-resistance or insulated interface splits the THERMAL model only: every node used by elements of both bodies
 * gets a second thermal node for the elements of body b, and each node on an interface face is joined to its twin by a
 * conductance pair whose area is the sum of a quarter of every interface face around it (row-sum lumping of the
 * interface integral). The total conductance is therefore h times the interface area, independent of the number of
 * nodes. The mechanical model keeps the shared nodes, so the bodies stay bonded structurally.
 *
 * Supported: coincident, face-matching interfaces between exactly two bodies. Refused with a diagnostic: bodies that
 * share no face (a gap, or contact only along edges), bodies whose geometry overlaps (the mesher gives contested cells
 * to one body, so the interface position is not defined by the geometry), and a finite-resistance interface that a
 * third body or the build plate also touches. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "../fem/thermal.h"
#include "../geom/hexmesh.h"
#include "setup.h"

typedef struct ContactTopology {
    int body_a, body_b;        /* mesh body indices */
    int nfaces;                /* interface faces */
    int *elem_a, *elem_b;      /* nfaces: the element of each body across every interface face */
    unsigned char *face_a;     /* nfaces: local face of elem_a that faces elem_b */
    double area;               /* m^2 */
    int nshared;               /* mesh nodes used by elements of both bodies */
    int *shared;               /* nshared, sorted */
    int nface_nodes;           /* shared nodes on interface faces */
    int njunction;             /* shared nodes that a third body or the plate also uses */
    int overlap_cells;         /* cells whose centre lies inside both bodies */
    double bmin[3], bmax[3];   /* bounding box of the interface faces (m) */
    double normal[3];          /* area-weighted unit normal pointing from a to b */
    double gap;                /* m: when nfaces == 0, the separation of the bodies' element boxes (0 when they meet) */
} ContactTopology;

/* analyses the pair; false only when out of memory. nfaces == 0 means the bodies do not share a face. */
bool contact_topology(const HexMesh *hm, int body_a, int body_b, ContactTopology *t);
void contact_topology_free(ContactTopology *t);
JsonValue *contact_topology_json(const ContactTopology *t);
/* NULL when the pair can carry an interface of this model, otherwise a sentence saying why not (written into buf) */
const char *contact_topology_problem(const ContactTopology *t, ContactModel model, const char *name_a, const char *name_b, char *buf, size_t cap);

/* The thermal split of a whole mesh for a set of interfaces (thermal_model.c uses it before resolving conditions).
 * xyz/conn are the case's copies (grown in place); tnode_mesh maps every thermal node to its mesh node. */
typedef struct ContactSplit {
    int nnodes;                 /* thermal nodes after the split */
    double *xyz;                /* 3 * nnodes */
    int *tnode_mesh;            /* nnodes */
    int npairs;
    ThermalInterfaceNode *pairs;
    int *pair_contact;          /* npairs: index of the interface each pair belongs to */
    double contact_area[THERMAL_MAX_GROUPS]; /* m^2 per interface */
    int contact_faces[THERMAL_MAX_GROUPS];
} ContactSplit;

/* splits the nodes shared by the two bodies of every non-perfect interface (contacts[i] with body indices bodies_a/b[i])
 * and builds the conductance pairs; conn (8 * nelems) is rewritten for the elements of each body b. */
bool contact_split(const HexMesh *hm, const double *xyz_in, int *conn, int ncontacts, const ThermalContact *contacts, const int *bodies_a,
                   const int *bodies_b, ContactSplit *out, char *err, size_t errlen);
void contact_split_free(ContactSplit *s);
