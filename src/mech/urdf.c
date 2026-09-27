/* urdf.c - URDF subset import into an Assembly
 *
 * Supported (URDF units are SI: metres, radians, kilograms)
 *   robot@name
 *   link@name: inertial (origin xyz rpy, mass value, inertia ixx ixy ixz iyy iyz izz); visual and collision (origin,
 *              geometry box@size | cylinder@radius,length | sphere@radius | mesh@filename,scale)
 *   joint@name,type = fixed | revolute | continuous | prismatic | floating: parent@link, child@link, origin xyz rpy,
 *              axis xyz, limit lower upper effort velocity, dynamics damping friction, mimic joint multiplier offset
 * Reported as unsupported (listed in the assembly, never silently dropped): planar joints, calibration,
 * safety_controller, transmission, gazebo, material colours, and any unknown element.
 * The root link has no joint in URDF; its connection to the world ("fixed" or "free") is an explicit import option and
 * a missing input otherwise. Links without an inertial element must be fixed to a parent (they become frames). Mesh
 * filenames: relative to the URDF file, file:// URIs, or package://name/ resolved through the options. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/sha256.h"
#include "assembly.h"
#include "xml.h"

typedef struct UCtx {
    Assembly *a;
    MechDiag *d;
    const AsmLoadOptions *opt;
    const char *base_dir;
    int errors;
} UCtx;

static bool numbers(UCtx *c, const XmlNode *n, const char *attr, double *out, int count, bool required, const double *def) {
    const char *s = xml_attr(n, attr);
    if (!s) {
        if (def) memcpy(out, def, (size_t)count * sizeof *out);
        if (required) {
            mdiag_add(c->d, MD_ERROR, "URDF_ATTRIBUTE_MISSING", n->name, NULL, "line %d: <%s> needs attribute '%s'", n->line, n->name, attr);
            c->errors++;
        }
        return !required;
    }
    char *end;
    const char *p = s;
    for (int i = 0; i < count; i++) {
        out[i] = strtod(p, &end);
        if (end == p || !isfinite(out[i])) {
            mdiag_add(c->d, MD_ERROR, "URDF_NUMBER", n->name, NULL, "line %d: attribute %s=\"%s\" needs %d number(s)", n->line, attr, s, count);
            c->errors++;
            return false;
        }
        p = end;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p) {
        mdiag_add(c->d, MD_ERROR, "URDF_NUMBER", n->name, NULL, "line %d: attribute %s=\"%s\" has extra content", n->line, attr, s);
        c->errors++;
        return false;
    }
    return true;
}

static void origin(UCtx *c, const XmlNode *parent, MPose *T) {
    mpose_identity(T);
    const XmlNode *o = xml_child(parent, "origin");
    if (!o) return;
    static const double zero[3] = {0, 0, 0};
    double xyz[3], rpy[3];
    numbers(c, o, "xyz", xyz, 3, false, zero);
    numbers(c, o, "rpy", rpy, 3, false, zero);
    mv3_copy(T->p, xyz);
    mrot_from_rpy(T->R, rpy[0], rpy[1], rpy[2]);
}

static void resolve_mesh(UCtx *c, const char *fn, char *out, size_t cap) {
    out[0] = 0;
    char tmp[NV_PATH_MAX];
    if (!strncmp(fn, "package://", 10)) {
        const char *rest = fn + 10, *slash = strchr(rest, '/');
        if (!slash) return;
        for (int i = 0; c->opt && i < c->opt->npackages; i++)
            if (strlen(c->opt->package_names[i]) == (size_t)(slash - rest) && !strncmp(c->opt->package_names[i], rest, (size_t)(slash - rest))) {
                snprintf(tmp, sizeof tmp, "%s%s", c->opt->package_dirs[i], slash);
                if (path_is_file(tmp)) snprintf(out, cap, "%s", tmp);
                return;
            }
        return;
    }
    if (!strncmp(fn, "file://", 7))
        snprintf(tmp, sizeof tmp, "%s", fn + 7);
    else if (fn[0] == '/')
        snprintf(tmp, sizeof tmp, "%s", fn);
    else if (!path_join(tmp, sizeof tmp, c->base_dir ? c->base_dir : ".", fn))
        return;
    if (path_is_file(tmp)) snprintf(out, cap, "%s", tmp);
}

static void read_geometry(UCtx *c, AsmBody *B, const XmlNode *vis, const char *role) {
    const XmlNode *g = xml_child(vis, "geometry");
    if (!g || g->nchildren != 1) {
        mdiag_add(c->d, MD_WARNING, "URDF_GEOMETRY", B->name, NULL, "line %d: <%s> needs exactly one geometry; skipped", vis->line, role);
        return;
    }
    AsmGeom *arr = realloc(B->geoms, (size_t)(B->ngeoms + 1) * sizeof *arr);
    if (!arr) return;
    B->geoms = arr;
    AsmGeom *G = &B->geoms[B->ngeoms];
    memset(G, 0, sizeof *G);
    snprintf(G->role, sizeof G->role, "%s", role);
    origin(c, vis, &G->pose);
    G->scale[0] = G->scale[1] = G->scale[2] = 1;
    const XmlNode *s = g->children[0];
    if (!strcmp(s->name, "box")) {
        G->type = AG_BOX;
        numbers(c, s, "size", G->size, 3, true, NULL);
    } else if (!strcmp(s->name, "sphere")) {
        G->type = AG_SPHERE;
        numbers(c, s, "radius", G->size, 1, true, NULL);
    } else if (!strcmp(s->name, "cylinder")) {
        G->type = AG_CYLINDER;
        numbers(c, s, "radius", G->size, 1, true, NULL);
        numbers(c, s, "length", G->size + 1, 1, true, NULL);
    } else if (!strcmp(s->name, "mesh")) {
        G->type = AG_MESH;
        const char *fn = xml_attr(s, "filename");
        snprintf(G->file_as_given, sizeof G->file_as_given, "%s", fn ? fn : "");
        static const double one[3] = {1, 1, 1};
        numbers(c, s, "scale", G->scale, 3, false, one);
        snprintf(G->units, sizeof G->units, "m");
        if (fn) resolve_mesh(c, fn, G->file, sizeof G->file);
        if (!G->file[0])
            mdiag_add(c->d, MD_WARNING, "GEOMETRY_FILE_MISSING", B->name, "for package:// paths pass the package directory in the import options",
                      "line %d: mesh '%s' not found", s->line, fn ? fn : "");
    } else {
        asm_note_unsupported(c->a, B->name, "geometry <%s> (line %d) is not supported", s->name, s->line);
        return;
    }
    B->ngeoms++;
}

static void unsupported_children(UCtx *c, const XmlNode *n, const char *subject, const char *const *known) {
    for (int i = 0; i < n->nchildren; i++) {
        const XmlNode *ch = n->children[i];
        bool ok = false;
        for (int k = 0; known[k] && !ok; k++) ok = !strcmp(ch->name, known[k]);
        if (!ok) {
            asm_note_unsupported(c->a, subject, "<%s> inside <%s> (line %d) is not imported", ch->name, n->name, ch->line);
            mdiag_add(c->d, MD_WARNING, "URDF_UNSUPPORTED", subject, NULL, "<%s> (line %d) is not imported", ch->name, ch->line);
        }
    }
}

Assembly *asm_from_urdf_text(const char *text, size_t len, const char *base_dir, const AsmLoadOptions *opt, MechDiag *d) {
    char err[256];
    XmlNode *root = xml_parse(text, len, err, sizeof err);
    if (!root) {
        mdiag_add(d, MD_ERROR, "URDF_XML", "", NULL, "%s", err);
        return NULL;
    }
    if (strcmp(root->name, "robot")) {
        mdiag_add(d, MD_ERROR, "URDF_ROOT", "", NULL, "root element is <%s>, expected <robot>", root->name);
        xml_free(root);
        return NULL;
    }
    const char *rname = xml_attr(root, "name");
    char safe[MB_NAME];
    snprintf(safe, sizeof safe, "%s", rname && *rname ? rname : "robot");
    for (char *p = safe; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) *p = '_';
    Assembly *a = asm_new(safe);
    UCtx c = {a, d, opt, base_dir, 0};
    snprintf(a->source_format, sizeof a->source_format, "urdf");
    asm_note_assumption(a, "gravity", SRC_DEFAULT, "URDF has no gravity: 9.80665 m/s^2 along -Z");
    /* links */
    for (int i = 0; i < root->nchildren; i++) {
        const XmlNode *n = root->children[i];
        if (strcmp(n->name, "link")) continue;
        const char *ln = xml_attr(n, "name");
        char nm[MB_NAME];
        snprintf(nm, sizeof nm, "%s", ln ? ln : "");
        bool valid = ln && *ln && strlen(ln) < MB_NAME;
        for (char *p = nm; valid && *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) valid = false;
        if (!valid || !strcmp(nm, "world")) {
            mdiag_add(d, MD_ERROR, "URDF_NAME", ln ? ln : "", "use letters, digits, '_' and '-' (up to 63 characters); 'world' is reserved",
                      "line %d: link name '%s' is not supported", n->line, ln ? ln : "");
            c.errors++;
            continue;
        }
        AsmBody *B = asm_add_body(a, nm);
        if (!B) {
            mdiag_add(d, MD_ERROR, "DUPLICATE_NAME", nm, NULL, "line %d: link '%s' defined twice", n->line, nm);
            c.errors++;
            continue;
        }
        static const char *const LK[] = {"inertial", "visual", "collision", NULL};
        unsupported_children(&c, n, nm, LK);
        const XmlNode *in = xml_child(n, "inertial");
        if (in) {
            MPose T;
            origin(&c, in, &T);
            const XmlNode *mass = xml_child(in, "mass"), *inert = xml_child(in, "inertia");
            double m = 0, I6[6] = {0};
            if (!mass || !numbers(&c, mass, "value", &m, 1, true, NULL) || !inert) {
                mdiag_add(d, MD_ERROR, "URDF_INERTIAL", nm, NULL, "line %d: <inertial> needs <mass value> and <inertia>", in->line);
                c.errors++;
                continue;
            }
            static const char *const IN[6] = {"ixx", "iyy", "izz", "ixy", "ixz", "iyz"};
            for (int k = 0; k < 6; k++) numbers(&c, inert, IN[k], I6 + k, 1, true, NULL);
            double Il[9] = {I6[0], I6[3], I6[4], I6[3], I6[1], I6[5], I6[4], I6[5], I6[2]};
            B->has_inertial = true;
            B->mass = m;
            mv3_copy(B->com, T.p);
            inertia_rotate(T.R, Il, B->inertia);
            B->inertial_source = SRC_USER;
            snprintf(B->inertial_note, sizeof B->inertial_note, "URDF <inertial> (line %d); its accuracy is that of the file", in->line);
        }
        for (int k = 0; k < n->nchildren; k++) {
            const XmlNode *ch = n->children[k];
            if (!strcmp(ch->name, "visual") || !strcmp(ch->name, "collision")) {
                read_geometry(&c, B, ch, ch->name);
                if (!strcmp(ch->name, "visual") && xml_child(ch, "material"))
                    mdiag_add(d, MD_INFO, "URDF_MATERIAL_IGNORED", nm, NULL, "line %d: visual material colour ignored", ch->line);
            }
        }
    }
    /* joints */
    int *parent_count = calloc((size_t)(a->nbodies ? a->nbodies : 1), sizeof *parent_count);
    for (int i = 0; i < root->nchildren && parent_count; i++) {
        const XmlNode *n = root->children[i];
        if (strcmp(n->name, "joint")) continue;
        const char *jn = xml_attr(n, "name"), *type = xml_attr(n, "type");
        const XmlNode *pn = xml_child(n, "parent"), *cn = xml_child(n, "child");
        const char *pl = pn ? xml_attr(pn, "link") : NULL, *cl = cn ? xml_attr(cn, "link") : NULL;
        int pi = asm_body_index(a, pl), ci = asm_body_index(a, cl);
        if (!jn || !type || pi < 0 || ci < 0) {
            mdiag_add(d, MD_ERROR, "URDF_JOINT", jn ? jn : "", NULL, "line %d: joint needs name, type and existing parent and child links", n->line);
            c.errors++;
            continue;
        }
        MbJointType jt;
        bool continuous = false;
        if (!strcmp(type, "fixed")) jt = MB_FIXED;
        else if (!strcmp(type, "revolute")) jt = MB_REVOLUTE;
        else if (!strcmp(type, "continuous")) jt = MB_REVOLUTE, continuous = true;
        else if (!strcmp(type, "prismatic")) jt = MB_PRISMATIC;
        else if (!strcmp(type, "floating")) jt = MB_FREE;
        else {
            asm_note_unsupported(a, jn, "joint type '%s' (line %d) is not supported", type, n->line);
            mdiag_add(d, MD_ERROR, "URDF_UNSUPPORTED_JOINT", jn, "planar joints are not available; model them with two prismatic joints and a revolute joint",
                      "line %d: joint type '%s' is not supported", n->line, type);
            c.errors++;
            continue;
        }
        if (++parent_count[ci] > 1) {
            mdiag_add(d, MD_ERROR, "URDF_NOT_A_TREE", cl, "URDF describes trees; close loops in the native format", "link '%s' has more than one parent joint",
                      cl);
            c.errors++;
            continue;
        }
        AsmJoint *AJ = asm_add_joint(a, jn, jt, pi, ci);
        if (!AJ) {
            mdiag_add(d, MD_ERROR, "DUPLICATE_NAME", jn, NULL, "line %d: joint '%s' defined twice", n->line, jn);
            c.errors++;
            continue;
        }
        MbJointDef *J = &AJ->def;
        origin(&c, n, &J->parent_frame);
        static const char *const JK[] = {"parent", "child", "origin", "axis", "limit", "dynamics", "mimic", NULL};
        unsupported_children(&c, n, jn, JK);
        if (jt == MB_REVOLUTE || jt == MB_PRISMATIC) {
            const XmlNode *ax = xml_child(n, "axis");
            static const double xaxis[3] = {1, 0, 0};
            if (ax)
                numbers(&c, ax, "xyz", J->axis, 3, true, NULL);
            else {
                mv3_copy(J->axis, xaxis);
                asm_note_assumption(a, jn, SRC_DEFAULT, "URDF axis omitted: the URDF default (1, 0, 0) is used");
            }
            double nrm = mv3_norm(J->axis);
            if (nrm > 0) mv3_scale(J->axis, J->axis, 1 / nrm);
            const XmlNode *lim = xml_child(n, "limit");
            if (lim) {
                double v;
                if (xml_attr(lim, "effort") && numbers(&c, lim, "effort", &v, 1, false, NULL)) AJ->effort_limit = v;
                if (xml_attr(lim, "velocity") && numbers(&c, lim, "velocity", &v, 1, false, NULL)) AJ->velocity_limit = v;
                if (!continuous) {
                    double lo = 0, hi = 0;
                    bool has = xml_attr(lim, "lower") || xml_attr(lim, "upper");
                    numbers(&c, lim, "lower", &lo, 1, false, (double[1]){0});
                    numbers(&c, lim, "upper", &hi, 1, false, (double[1]){0});
                    if (has && hi > lo) {
                        J->limited = true, J->lower = lo, J->upper = hi, J->restitution = 0;
                        asm_note_assumption(a, jn, SRC_DEFAULT, "URDF limits carry no impact law: restitution 0 (the joint stops at the limit)");
                    } else if (!has)
                        asm_note_assumption(a, jn, SRC_DEFAULT, "URDF limit without lower/upper: unlimited range");
                }
            } else if (!continuous) {
                mdiag_add(d, MD_WARNING, "URDF_LIMIT_MISSING", jn, NULL, "line %d: revolute/prismatic joint without <limit>: imported without range limits",
                          n->line);
                asm_note_assumption(a, jn, SRC_DEFAULT, "no <limit>: unlimited range, no effort or velocity rating");
            }
            const XmlNode *dyn = xml_child(n, "dynamics");
            if (dyn) {
                double v;
                if (xml_attr(dyn, "damping") && numbers(&c, dyn, "damping", &v, 1, false, NULL)) J->damping = v;
                if (xml_attr(dyn, "friction") && numbers(&c, dyn, "friction", &v, 1, false, NULL)) J->coulomb = v;
            } else
                asm_note_assumption(a, jn, SRC_DEFAULT, "no <dynamics>: ideal frictionless, undamped joint");
        }
        J->motion = MB_PASSIVE;
        asm_note_assumption(a, jn, SRC_DEFAULT, "URDF import: joint is passive until an actuator is attached");
        const XmlNode *mimic = xml_child(n, "mimic");
        if (mimic) {
            const char *drv = xml_attr(mimic, "joint");
            double mult = 1, off = 0;
            numbers(&c, mimic, "multiplier", &mult, 1, false, (double[1]){1});
            numbers(&c, mimic, "offset", &off, 1, false, (double[1]){0});
            AsmCoupling *C = realloc(a->couplings, (size_t)(a->ncouplings + 1) * sizeof *C);
            if (C && drv) {
                a->couplings = C;
                a->cap_couplings = a->ncouplings + 1;
                AsmCoupling *X = &a->couplings[a->ncouplings++];
                memset(X, 0, sizeof *X);
                snprintf(X->name, sizeof X->name, "mimic_%s", jn);
                snprintf(X->follower, sizeof X->follower, "%s", jn);
                snprintf(X->driver, sizeof X->driver, "%s", drv);
                X->ratio = mult, X->offset = off;
            } else if (C)
                a->couplings = C;
        }
    }
    /* top-level elements that are not links or joints */
    for (int i = 0; i < root->nchildren; i++) {
        const XmlNode *n = root->children[i];
        if (!strcmp(n->name, "link") || !strcmp(n->name, "joint")) continue;
        if (!strcmp(n->name, "material")) continue; /* colour definitions */
        asm_note_unsupported(a, n->name, "<%s> (line %d) is not imported%s", n->name, n->line,
                             !strcmp(n->name, "transmission") ? ": attach actuators and transmissions explicitly" : "");
        mdiag_add(d, MD_WARNING, "URDF_UNSUPPORTED", n->name, NULL, "<%s> (line %d) is not imported", n->name, n->line);
    }
    /* mimic couplings must refer to existing joints */
    for (int k = 0; k < a->ncouplings; k++)
        if (asm_joint_index(a, a->couplings[k].driver) < 0) {
            mdiag_add(d, MD_ERROR, "URDF_MIMIC", a->couplings[k].follower, NULL, "mimic refers to unknown joint '%s'", a->couplings[k].driver);
            c.errors++;
        }
    /* root link and its connection to the world */
    int roots = 0, root_link = -1;
    for (int b = 0; parent_count && b < a->nbodies; b++)
        if (parent_count[b] == 0) roots++, root_link = b;
    free(parent_count);
    if (roots != 1 && !c.errors) {
        mdiag_add(d, MD_ERROR, "URDF_ROOT", "", NULL, "expected exactly one root link, found %d", roots);
        c.errors++;
    } else if (root_link >= 0 && !c.errors) {
        const char *rj = opt ? opt->root_joint : NULL;
        if (!rj) {
            mdiag_add(d, MD_MISSING_INPUT, "ROOT_CONNECTION", a->bodies[root_link].name,
                      "choose root_joint \"fixed\" (bolted to the world) or \"free\" (a floating base): URDF does not say",
                      "how is root link '%s' connected to the world?", a->bodies[root_link].name);
            c.errors++;
        } else if (!strcmp(rj, "fixed") || !strcmp(rj, "free")) {
            char jn[MB_NAME];
            snprintf(jn, sizeof jn, "world_to_%.50s", a->bodies[root_link].name);
            AsmJoint *AJ = asm_add_joint(a, jn, !strcmp(rj, "fixed") ? MB_FIXED : MB_FREE, -1, root_link);
            if (AJ) asm_note_assumption(a, jn, SRC_USER, "root link connected to the world by a %s joint at the world origin", rj);
        } else {
            mdiag_add(d, MD_ERROR, "ROOT_CONNECTION", "", NULL, "root_joint must be \"fixed\" or \"free\"");
            c.errors++;
        }
    }
    xml_free(root);
    if (c.errors) {
        asm_free(a);
        return NULL;
    }
    return a;
}

Assembly *asm_load_urdf(const char *path, const AsmLoadOptions *opt, MechDiag *d) {
    size_t len;
    char err[256];
    size_t maxb = opt && opt->max_file_bytes ? (size_t)opt->max_file_bytes : (size_t)16 << 20;
    char *text = path_read_file(path, maxb, &len, err, sizeof err);
    if (!text) {
        mdiag_add(d, MD_ERROR, "URDF_READ", path, NULL, "%s", err);
        return NULL;
    }
    char dir[NV_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    Assembly *a = asm_from_urdf_text(text, len, dir, opt, d);
    free(text);
    if (a) {
        snprintf(a->source_path, sizeof a->source_path, "%s", path);
        uint64_t sz;
        sha256_file(path, a->source_sha256, &sz);
    }
    return a;
}
