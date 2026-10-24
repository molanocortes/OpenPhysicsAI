/* mag3d_mesh.hpp - the cylindrical grid of mag3d.h as an MFEM mesh, shared by the magnetostatic, eddy-current and heat
 * solvers (mag3d.cpp, eddy3d.cpp). C++ only. */
#pragma once
#include "mag3d.h"
#include "mfem.hpp"

mfem::Mesh *mag3d_build_mesh(const Mag3DGrid *g, mfem::Mesh &mesh, mfem::Mesh &periodic);
