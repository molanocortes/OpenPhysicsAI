# src/lab/lab.mk - build fragment of the physics lab (src/lab/*), included by the top-level Makefile after mech.mk.
# The lab solvers join the headless core, so the servers, the tests and the app all link them; each domain adds its
# verification binary to LAB_TESTS. Numerical code: IEEE, no fast-math (CORE_CFLAGS).
# MFEM (BSD-3-Clause, docs/THIRD_PARTY.md): fetched, checked and built by tools/build_mfem.py on first use (make mfem),
# serial, double precision, no fast-math. With it built, every lab binary and the app carry the 3D finite-element
# backends (3D magnetics); without it the lab builds and runs as before and a 3D magnetic scenario says what to build.
MFEM_DIR := build/third_party/mfem-4.8
MFEM_LIB := $(MFEM_DIR)/libmfem.a
SDK_CXX := $(shell d=$$(xcrun --show-sdk-path 2>/dev/null)/usr/include/c++/v1; [ -d $$d ] && echo -isystem $$d)
MFEM_CXXFLAGS := -std=c++17 -O2 -ffp-contract=off -Wall -Wno-unused-parameter $(SDK_CXX) -I$(MFEM_DIR)
MFEM_ONLY_C := src/lab/magnet/spdirect.c src/lab/magnet/motor3d.c src/lab/magnet/induct3d.c
MAG3D_OBJ := build/obj/lab/magnet/mag3d.o build/obj/lab/magnet/eddy3d.o build/obj/lab/magnet/spdirect.o
LAB3D_OBJ := $(MAG3D_OBJ) build/obj/lab/magnet/motor3d.o build/obj/lab/magnet/induct3d.o
# the GPU flow engine (Metal, a system framework of macOS): Objective-C around Metal Shading Language kernels
ifeq ($(shell uname -s),Darwin)
CORE_OBJ += build/obj/lab/flow/lbm3d_metal.o build/obj/lab/water/water_metal.o
CORE_LDLIBS += -framework Metal -framework Foundation
LDFLAGS += -framework Metal
endif
ifneq ($(wildcard $(MFEM_LIB)),)
ifeq ($(shell uname -s),Darwin)
CORE_OBJ += $(LAB3D_OBJ)
CORE_LDLIBS += $(MFEM_LIB) -lc++ -framework Accelerate
LDFLAGS += $(MFEM_LIB) -lc++ -framework Accelerate
endif
endif
LAB_SRC := $(filter-out $(MFEM_ONLY_C),$(wildcard src/lab/*.c) $(wildcard src/lab/*/*.c))
CORE_SRC += $(LAB_SRC)
CORE_OBJ += $(patsubst src/%.c,build/obj/%.o,$(LAB_SRC))
LAB_TESTS := build/poissontest build/firetest build/roomtest build/battest build/fsitest build/melttest build/sheettest build/peritest build/euler3dtest build/lbm3dtest build/gas3dtest build/flow3dtest build/heat3dtest build/labwatertest build/labvoltest build/labscenetest build/labtest build/gastest build/actest build/imptest build/orbtest build/emtest build/flowtest build/sphtest build/hftest build/mgtest build/rttest build/wtest
LAB_TOOLS := build/labfilm build/labprobe build/labrun
ifneq ($(wildcard $(MFEM_LIB)),)
LAB3D_TESTS := build/magnet3dtest build/mag3dtest build/motor3dtest build/eddy3dtest
endif

build/labtest: tools/labtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/gastest: tools/gastest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/actest: tools/actest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/imptest: tools/imptest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/orbtest: tools/orbtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/emtest: tools/emtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/flowtest: tools/flowtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/sphtest: tools/sphtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/hftest: tools/hftest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/mgtest: tools/mgtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/rttest: tools/rttest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/wtest: tools/wtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/labfilm: tools/labfilm.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/labprobe: tools/labprobe.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/labrun: tools/labrun.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

.PHONY: test-lab lab
lab: $(LAB_TESTS) $(LAB3D_TESTS) $(LAB_TOOLS)
test-lab: $(LAB_TESTS) $(LAB3D_TESTS)
	$(if $(LAB3D_TESTS),$(MAKE) test-lab3d)
	./build/lbm3dtest
	./build/euler3dtest
	./build/peritest
	./build/sheettest
	./build/melttest
	./build/fsitest
	./build/poissontest
	./build/firetest
	./build/roomtest
	./build/battest
	./build/gas3dtest
	./build/flow3dtest
	./build/heat3dtest
	./build/labwatertest
	./build/labvoltest
	./build/labscenetest
	./build/labtest
	./build/gastest
	./build/actest
	./build/imptest
	./build/orbtest
	./build/emtest
	./build/flowtest
	./build/sphtest
	./build/hftest
	./build/mgtest
	./build/rttest
	./build/wtest
headless: $(LAB_TESTS) $(LAB_TOOLS)

build/labscenetest: tools/labscenetest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/labvoltest: tools/labvoltest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/labwatertest: tools/labwatertest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/heat3dtest: tools/heat3dtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/flow3dtest: tools/flow3dtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/gas3dtest: tools/gas3dtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

$(MFEM_LIB): tools/build_mfem.py
	python3 tools/build_mfem.py
.PHONY: mfem
mfem: $(MFEM_LIB)
build/obj/lab/magnet/magnet3d.o: src/lab/magnet/magnet3d.cpp src/lab/magnet/magnet3d.h $(MFEM_LIB)
	@mkdir -p $(dir $@)
	clang++ $(MFEM_CXXFLAGS) -c $< -o $@
build/magnet3dtest: tools/magnet3dtest.cpp build/obj/lab/magnet/magnet3d.o $(MFEM_LIB)
	clang++ $(MFEM_CXXFLAGS) $< build/obj/lab/magnet/magnet3d.o $(MFEM_LIB) -o $@
build/obj/lab/magnet/mag3d.o: src/lab/magnet/mag3d.cpp src/lab/magnet/mag3d.h src/lab/magnet/spdirect.h $(MFEM_LIB)
	@mkdir -p $(dir $@)
	clang++ $(MFEM_CXXFLAGS) -c $< -o $@
build/obj/lab/magnet/eddy3d.o: src/lab/magnet/eddy3d.cpp src/lab/magnet/eddy3d.h src/lab/magnet/mag3d.h src/lab/magnet/mag3d_mesh.hpp src/lab/magnet/spdirect.h $(MFEM_LIB)
	@mkdir -p $(dir $@)
	clang++ $(MFEM_CXXFLAGS) -c $< -o $@
build/eddy3dtest: tools/eddy3dtest.cpp $(MAG3D_OBJ) $(MFEM_LIB)
	clang++ $(MFEM_CXXFLAGS) $< $(MAG3D_OBJ) $(MFEM_LIB) -framework Accelerate -o $@
build/obj/lab/magnet/spdirect.o: src/lab/magnet/spdirect.c src/lab/magnet/spdirect.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -ffp-contract=off -Wall -Wextra -c $< -o $@
build/obj/lab/magnet/motor3d.o: src/lab/magnet/motor3d.c src/lab/magnet/mag3d.h src/lab/magnet/drive.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -MMD -MP,$(CORE_CFLAGS)) -c $< -o $@
build/mag3dtest: tools/mag3dtest.cpp $(MAG3D_OBJ) $(MFEM_LIB)
	clang++ $(MFEM_CXXFLAGS) $< $(MAG3D_OBJ) $(MFEM_LIB) -framework Accelerate -o $@
build/motor3dtest: tools/motor3dtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
# the 3D finite-element checks, run by test-lab when MFEM is built
.PHONY: test-lab3d
test-lab3d: build/magnet3dtest build/mag3dtest build/motor3dtest build/eddy3dtest
	./build/magnet3dtest
	./build/eddy3dtest
	./build/mag3dtest
	./build/motor3dtest
build/lbm3dtest: tools/lbm3dtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/obj/lab/flow/lbm3d_metal.o: src/lab/flow/lbm3d_metal.m src/lab/flow/lbm3d_metal.h src/lab/flow/lbm3d.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -fobjc-arc -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -c $< -o $@
build/obj/lab/water/water_metal.o: src/lab/water/water_metal.m src/lab/water/water_metal.h src/lab/water/water.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -fobjc-arc -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -c $< -o $@
build/euler3dtest: tools/euler3dtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/peritest: tools/peritest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/obj/lab/magnet/induct3d.o: src/lab/magnet/induct3d.c src/lab/magnet/eddy3d.h src/lab/magnet/mag3d.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -MMD -MP,$(CORE_CFLAGS)) -c $< -o $@
build/sheettest: tools/sheettest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/melttest: tools/melttest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/fsitest: tools/fsitest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/poissontest: tools/poissontest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/firetest: tools/firetest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/roomtest: tools/roomtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
build/battest: tools/battest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)
