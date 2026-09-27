/* labio.h - the one result format of every physics-lab solver (the directories under src/lab): a header and a sequence of frames.
 *
 * A frame holds a time and parts. A part is one piece of geometry with its fields:
 *   LAB_BLOCKS  structured patches (an adaptive mesh is many blocks at different levels); cell-centred fields
 *   LAB_POINTS  particles, bodies or samples; per-point fields
 *   LAB_CELLS   an unstructured mesh (lines, triangles, quads, hexahedra); node or cell fields
 * Geometry is stored in double precision, fields in single precision (they are for looking at and for probing; the
 * numbers a verification test checks are computed from the solver's own double arrays, never read back from here).
 *
 * File layout, little endian: the 8 bytes "OPLAB01\n", a uint32 length and that many bytes of UTF-8 JSON (the header:
 * domain, title, units, the scenario that produced the run, the commit), then frames, each "FRAM", a uint64 byte count
 * and the frame's content. A reader indexes the frames by skipping over them, so a partly written file (a run that was
 * stopped) still opens with the frames it has.
 *
 * The header is written by the solver as text; the reader returns it as text. Units of every field are declared in
 * the header under "fields": {"name": "unit"}; labfilm and the app print them on the colour bar. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum { LAB_BLOCKS = 1, LAB_POINTS = 2, LAB_CELLS = 3 } LabPartKind;
typedef enum { LAB_AT_CELL = 0, LAB_AT_NODE = 1 } LabLocation; /* LAB_POINTS fields are LAB_AT_NODE */
typedef enum { LAB_LINE = 2, LAB_TRI = 3, LAB_QUAD = 4, LAB_HEX = 8 } LabCellType; /* the value is nodes per cell */
/* the plane a 2D block lies in when it is placed in 3D space */
typedef enum { LAB_PLANE_XY = 0, LAB_PLANE_XZ = 1, LAB_PLANE_YZ = 2 } LabPlane;

typedef struct LabBlock {
    int n[3];         /* cells per direction; n[2] = 1 for a 2D block */
    int level;        /* refinement level, 0 the coarsest */
    int plane;        /* LabPlane of a 2D block; ignored in 3D */
    double origin[3]; /* lower corner, m */
    double dx[3];     /* cell size, m */
} LabBlock;

typedef struct LabField {
    char name[48];
    int location;  /* LabLocation */
    size_t count;  /* values: cells over all blocks, points, nodes or cells */
    float *data;
} LabField;

typedef struct LabPart {
    int kind; /* LabPartKind */
    char name[48];
    /* LAB_BLOCKS */
    int nblocks;
    LabBlock *blocks;
    /* LAB_POINTS and LAB_CELLS: the nodes */
    int npoints;
    double *xyz;
    /* LAB_CELLS */
    int ncells, cell_type;
    int *conn;
    int nfields;
    LabField *fields;
} LabPart;

typedef struct LabFrame {
    double time; /* s */
    int nparts;
    LabPart *parts;
} LabFrame;

/* ---- writing ---- */
typedef struct LabWriter LabWriter;

LabWriter *lab_create(const char *path, const char *header_json, char *err, size_t errlen);
void lab_frame_begin(LabWriter *w, double time);
/* each part call starts a new part; lab_field attaches to the latest part */
void lab_part_blocks(LabWriter *w, const char *name, int nblocks, const LabBlock *blocks);
void lab_part_points(LabWriter *w, const char *name, int npoints, const double *xyz);
void lab_part_cells(LabWriter *w, const char *name, int nnodes, const double *xyz, int ncells, int cell_type, const int *conn);
void lab_field(LabWriter *w, const char *name, int location, size_t count, const float *data);
void lab_field_d(LabWriter *w, const char *name, int location, size_t count, const double *data); /* converts */
bool lab_frame_end(LabWriter *w); /* false on a write error (disk full); the file keeps the frames before */
bool lab_close(LabWriter *w);     /* false if any write failed */
int lab_frames_written(const LabWriter *w);

/* ---- reading ---- */
typedef struct LabFile LabFile;

LabFile *lab_open(const char *path, char *err, size_t errlen);
void lab_close_file(LabFile *f);
const char *lab_header(const LabFile *f); /* NUL-terminated JSON text */
int lab_frame_count(const LabFile *f);
double lab_frame_time(const LabFile *f, int i);
/* reads frame i into fr (fr is cleared first); free with lab_frame_free */
bool lab_read_frame(LabFile *f, int i, LabFrame *fr, char *err, size_t errlen);
void lab_frame_free(LabFrame *fr);

/* helpers */
const LabPart *lab_find_part(const LabFrame *fr, const char *name);
const LabField *lab_find_field(const LabPart *p, const char *name);
size_t lab_block_cells(const LabBlock *b);
size_t lab_part_ncells(const LabPart *p); /* all cells of a blocks part */
