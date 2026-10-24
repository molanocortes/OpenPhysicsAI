# cad.mk - printed-part geometry (GOALS.md G23, docs/cad.md): Open CASCADE 7.9.3 (LGPL-2.1 with its exception, the
# owner's decision of 2026-10-08) and Manifold 3.5.4 (Apache-2.0), fetched, checked and built static by
# tools/build_cad.py (make cad; about an hour the first time on a machine). They are linked only into build/openphysics-cad
# and the tests here, never into the app or openphysics-mcp, which run openphysics-cad as a program of their own; plain `make`
# builds it with everything else. Built once per machine, in one place every copy of the repository shares.
CAD_TP ?= $(HOME)/.openphysicsai/third_party
OCCT_DIR := $(CAD_TP)/occt-7.9.3
MANIFOLD_DIR := $(CAD_TP)/manifold-3.5.4
# written last, after Manifold's (GNU make 3.81 has no grouped targets); a build from before the rename of 2026-10-21 left
# .navier-built, which counts until tools/build_cad.py writes the new one
CAD_STAMP := $(firstword $(wildcard $(OCCT_DIR)/.openphysics-built $(OCCT_DIR)/.navier-built) $(OCCT_DIR)/.openphysics-built)
CAD_SDK_CXX := $(shell d=$$(xcrun --show-sdk-path 2>/dev/null)/usr/include/c++/v1; [ -d $$d ] && echo -isystem $$d)
CAD_CXXFLAGS := -std=c++17 -O2 -ffp-contract=off -Wall -Wno-unused-parameter $(CAD_SDK_CXX) -isystem $(OCCT_DIR)/include/opencascade \
	-isystem $(MANIFOLD_DIR)/include
# every toolkit installed. ld64 searches static archives until nothing more resolves, so their order does not matter;
# GNU ld reads each archive once, in order, so on Linux the toolkits, which use one another, go in one group
ifeq ($(UNAME),Darwin)
CAD_LIBS = $(wildcard $(OCCT_DIR)/lib/libTK*.a) $(MANIFOLD_DIR)/lib/libmanifold.a -framework Foundation -framework AppKit \
	-framework IOKit
else
CAD_LIBS = -Wl,--start-group $(wildcard $(OCCT_DIR)/lib/libTK*.a) $(MANIFOLD_DIR)/lib/libmanifold.a -Wl,--end-group -lpthread -ldl
endif
CAD_CORE := build/obj/core/json.o build/obj/core/sbuf.o build/obj/core/errors.o build/obj/geom/bvh.o
# the support module's periodic cell solver and what it needs (G23 step 5: a lattice core's cell homogenised)
CAD_CELL := build/obj/mech/support_cell.o build/obj/fem/solid.o build/obj/fem/hex8.o build/obj/fem/dense.o build/obj/fem/sparse.o \
	build/obj/fem/krylov.o build/obj/fem/tet.o build/obj/threads.o build/obj/common.o

.PHONY: cad
cad: $(CAD_STAMP)
$(CAD_STAMP): tools/build_cad.py
	CAD_TP="$(CAD_TP)" python3 tools/build_cad.py

build/cadtest: tools/cadtest.cpp $(CAD_STAMP)
	@mkdir -p build
	clang++ $(CAD_CXXFLAGS) $< $(CAD_LIBS) -o $@

build/obj/cad/cadpart.o: src/cad/cadpart.cpp src/cad/cadpart.h $(CAD_STAMP)
	@mkdir -p $(dir $@)
	clang++ $(CAD_CXXFLAGS) -c $< -o $@

build/obj/cad/lattice.o: src/cad/lattice.cpp src/cad/lattice.h src/cad/cadmesh.h src/cad/specimen.h src/mech/support_cell.h $(CAD_STAMP)
	@mkdir -p $(dir $@)
	clang++ $(CAD_CXXFLAGS) -c $< -o $@

build/obj/cad/cadmesh.o: src/cad/cadmesh.cpp src/cad/cadmesh.h src/geom/bvh.h $(CAD_STAMP)
	@mkdir -p $(dir $@)
	clang++ $(CAD_CXXFLAGS) -c $< -o $@

build/obj/cad/printcheck.o: src/cad/printcheck.cpp src/cad/printcheck.h src/cad/cadmesh.h src/geom/bvh.h $(CAD_STAMP)
	@mkdir -p $(dir $@)
	clang++ $(CAD_CXXFLAGS) -c $< -o $@

build/obj/cad/specimen.o: src/cad/specimen.cpp src/cad/specimen.h src/fem/solid.h
	@mkdir -p $(dir $@)
	clang++ $(CAD_CXXFLAGS) -I. -c $< -o $@

CAD_OBJ := build/obj/cad/cadpart.o build/obj/cad/lattice.o build/obj/cad/cadmesh.o build/obj/cad/printcheck.o build/obj/cad/specimen.o
build/openphysics-cad: src/bin/openphysics_cad.cpp $(CAD_OBJ) $(CAD_CORE) $(CAD_CELL) $(CAD_STAMP)
	clang++ $(CAD_CXXFLAGS) $< $(CAD_OBJ) $(CAD_CORE) $(CAD_CELL) $(CAD_LIBS) -o $@

# S1 and S2 of G23 step 5c: the specimen between platens against closed forms (no Open CASCADE needed)
build/specimentest: tools/specimentest.cpp build/obj/cad/specimen.o $(CAD_CELL)
	clang++ $(CAD_CXXFLAGS) $< build/obj/cad/specimen.o $(CAD_CELL) -o $@

# H1 and H2 of G23 step 5: the cell solver's new entry against closed forms (no Open CASCADE needed)
build/cellstifftest: tools/cellstifftest.c $(CAD_CELL)
	$(CC) $(CORE_CFLAGS) -I. $< $(CAD_CELL) -lm -o $@

.PHONY: test-cad
test-cad: build/cadtest build/openphysics-cad build/cellstifftest build/specimentest
	./build/cadtest
	./build/cellstifftest
	./build/specimentest
	python3 tools/cadparttest.py
	python3 tools/cadlatticetest.py
	python3 tools/cadprinttest.py
	python3 tools/cadcelltest.py
