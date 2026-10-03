# NAVIER - real-time 3D lattice Boltzmann water tunnel (pure C, macOS) and the NAVIER-AM additive-manufacturing core
CC       := clang
UNAME    := $(shell uname -s)
CPUFLAG  := $(shell $(CC) -mcpu=native -E -x c /dev/null >/dev/null 2>&1 && echo -mcpu=native)
CFLAGS   := -std=c11 -O3 $(CPUFLAG) -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -pthread -MMD -MP
# code that must keep exact IEEE NaN/Inf semantics (file parsing, tests) is built without -ffast-math
NOFAST   := $(filter-out -ffast-math,$(CFLAGS))
# NAVIER-AM numerical core, control layer, servers and tests: IEEE semantics (no -ffast-math, so NaN checks and summation
# order are honoured) and no floating-point contraction (FMA), so results do not change between compilers and platforms
CORE_CFLAGS := -std=c11 -O2 $(CPUFLAG) -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -pthread -MMD -MP
ifeq ($(UNAME),Linux)
CFLAGS      += -D_GNU_SOURCE
CORE_CFLAGS += -D_GNU_SOURCE
endif
LDFLAGS  := -pthread -framework Cocoa -framework OpenGL -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework ImageIO
CORE_LDLIBS := -pthread -lm

SRC      := $(wildcard src/*.c) $(wildcard src/geom/*.c)
OBJ      := $(patsubst src/%.c,build/obj/%.o,$(SRC))
APP      := navier
BUNDLE   := NAVIER.app

# AM core: no OpenGL, Cocoa or UI state, so it also builds headless on Linux
CORE_SRC := $(wildcard src/core/*.c) $(wildcard src/ctl/*.c) $(wildcard src/net/*.c) $(wildcard src/fem/*.c) $(wildcard src/render/*.c) src/geom/mesh.c src/geom/bvh.c src/geom/surface.c src/geom/patches.c src/geom/hexmesh.c src/geom/repair.c src/geom/tetmesh.c
CORE_OBJ := $(patsubst src/%.c,build/obj/%.o,$(CORE_SRC)) build/obj/gen/ops_schema.o build/obj/gen/materials_data.o build/obj/common.o build/obj/threads.o build/obj/headless/lbm.o
-include src/mech/mech.mk
-include src/lab/lab.mk
# the lattice Boltzmann kernel is shared: the GUI links its own fast-math build (build/obj/lbm.o), the headless core an
# IEEE build of the same source (build/obj/headless/lbm.o), so the two never meet in one binary
GUI_OBJ := $(sort $(OBJ) $(filter-out build/obj/headless/lbm.o,$(CORE_OBJ)))
LINK_CFLAGS := $(filter-out -MMD -MP,$(CORE_CFLAGS))
AM_BINS  := navier-server navier-mcp navier-ctl
TESTS    := build/coretest build/surftest build/meshtest build/femtest build/rendertest build/opstest build/ctltest build/amtest build/thermtest build/tsteptest build/orchtest build/advtest build/evaltest build/tettest build/topotest

.PHONY: all app clean tools run test am headless
all: $(APP) $(AM_BINS)

am: $(AM_BINS)

# everything that builds without Cocoa/OpenGL (the Linux headless target)
headless: $(AM_BINS) $(TESTS)

navier-server: src/bin/navier_server.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

navier-mcp: src/bin/navier_mcp.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

navier-ctl: src/bin/navier_ctl.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

$(APP): $(GUI_OBJ)
	$(CC) $(GUI_OBJ) -o $@ $(LDFLAGS)

# the commit the app is built from, for the run records Agent mode writes; the header is rewritten only when it changes
build/obj/app.o: build/gen/app_commit.h
build/gen/app_commit.h: FORCE
	@mkdir -p $(dir $@); c=$$(git rev-parse --short HEAD 2>/dev/null || echo unknown); \
	git diff --quiet HEAD -- src 2>/dev/null || c="$$c with local changes"; \
	echo "#define NAVIER_APP_COMMIT \"$$c\"" > $@.tmp; cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
.PHONY: FORCE
FORCE:

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(patsubst src/%.c,build/obj/%.o,$(CORE_SRC)): CFLAGS := $(CORE_CFLAGS)

build/obj/headless/lbm.o: src/lbm.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -Wno-pass-failed -c $< -o $@

# the operation contract is a JSON file embedded into every binary that serves it
build/embed: tools/embed.c
	@mkdir -p build
	$(CC) -O1 $< -o $@

build/gen/ops_schema.c: src/ctl/ops_schema.json build/embed
	@mkdir -p build/gen
	./build/embed $< $@ NAVIER_OPS_SCHEMA

build/obj/gen/ops_schema.o: build/gen/ops_schema.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c $< -o $@

# the material library (demonstration values, labelled as such) is embedded the same way
build/gen/materials_data.c: src/ctl/materials.json build/embed
	@mkdir -p build/gen
	./build/embed $< $@ NAVIER_MATERIALS

build/obj/gen/materials_data.o: build/gen/materials_data.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c $< -o $@

app: $(APP)
	@mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	cp $(APP) $(BUNDLE)/Contents/MacOS/navier
	cp tools/Info.plist $(BUNDLE)/Contents/Info.plist
	@# Agent mode points the user's AI tool at navier-mcp beside the app; Simple mode opens the samples and presets
	@if [ -x navier-mcp ]; then cp navier-mcp $(BUNDLE)/Contents/MacOS/navier-mcp; fi
	@mkdir -p $(BUNDLE)/Contents/Resources/samples
	cp samples/*.stl samples/samples.json $(BUNDLE)/Contents/Resources/samples/
	cp src/ctl/print_profiles.json $(BUNDLE)/Contents/Resources/print_profiles.json
	@echo "built $(BUNDLE) - double-click it in Finder, files are saved to ~/Documents/NAVIER"

build/gltest: tools/gltest.c build/obj/platform_macos.o build/obj/glutil.o build/obj/image.o build/obj/font.o build/obj/common.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

build/lbmbench: tools/lbmbench.c build/obj/lbm.o build/obj/common.o build/obj/threads.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

build/geomtest: tools/geomtest.c build/obj/geom/mesh.o build/obj/geom/shapes.o build/obj/geom/voxel.o build/obj/common.o build/obj/threads.o
	$(CC) $(NOFAST) $^ -o $@ $(LDFLAGS)

build/vistest: tools/vistest.c build/obj/vis_fields.o build/obj/vis_stream.o build/obj/vis_iso.o build/obj/common.o build/obj/threads.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

tools: build/gltest build/lbmbench build/geomtest build/vistest

# ---- NAVIER-AM tests (headless) ----
build/coretest: tools/coretest.c $(patsubst src/%.c,build/obj/%.o,$(wildcard src/core/*.c))
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/surftest: tools/surftest.c build/obj/geom/mesh.o build/obj/geom/bvh.o build/obj/geom/surface.o build/obj/common.o
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/meshtest: tools/meshtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/femtest: tools/femtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/rendertest: tools/rendertest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/opstest: tools/opstest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/ctltest: tools/ctltest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/amtest: tools/amtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/thermtest: tools/thermtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/tsteptest: tools/tsteptest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/orchtest: tools/orchtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/advtest: tools/advtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/evaltest: tools/evaltest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/tettest: tools/tettest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/topotest: tools/topotest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

# headless test suite: unit tests, operation layer, control socket, MCP protocol and the two complete AI-driven
# workflows (structural: amflow; thermal, anisotropy and melting: thermflow), both with independent Python clients
test: $(TESTS) $(AM_BINS)
	./build/coretest
	./build/surftest
	./build/meshtest
	./build/femtest
	./build/thermtest
	./build/tsteptest
	./build/orchtest
	./build/advtest
	./build/evaltest
	./build/tettest
	./build/topotest
	./build/rendertest build/rendertest_out
	python3 tools/pngcheck.py build/rendertest_out
	./build/opstest
	./build/ctltest
	./build/amtest
	python3 tools/mcptest.py
	python3 tools/amflow.py
	python3 tools/thermflow.py
	python3 tools/transientflow.py
	python3 tools/restartflow.py
	python3 tools/contactflow.py
	python3 tools/chtflow.py
	python3 tools/studyflow.py
	./navier-ctl doctor

# the tier a session runs after each step (AGENTS.md rule 8): every C suite plus the MCP protocol test, no flow
# script, no doctor. Under three minutes on the development laptop; `make test` stays the full suite and is run
# detached with a log before a wave's final report and in CI.
.PHONY: test-fast
test-fast: $(TESTS) $(MECH_TESTS) $(LAB_TESTS) $(AM_BINS)
	./build/coretest
	./build/surftest
	./build/meshtest
	./build/femtest
	./build/thermtest
	./build/tsteptest
	./build/orchtest
	./build/advtest
	./build/evaltest
	./build/tettest
	./build/rendertest build/rendertest_out
	python3 tools/pngcheck.py build/rendertest_out
	./build/opstest
	./build/ctltest
	./build/amtest
	./build/mechtest
	./build/dyntest
	./build/poissontest
	./build/battest
	./build/melttest
	./build/sheettest
	./build/peritest
	./build/euler3dtest
	./build/labtest
	./build/labscenetest
	./build/lbm3dtest --fast
	./build/labvoltest
	./build/gas3dtest
	./build/flow3dtest
	./build/heat3dtest
	./build/labwatertest
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
	./build/wtest --fast
	python3 tools/mcptest.py
	python3 tools/mcp_http_test.py

# adds the interface checks: builds the app (Cocoa/OpenGL) and clicks every control off-screen
.PHONY: test-ui
test-ui: $(APP) build/femviewtest
	./build/femviewtest
	python3 tools/uicheck.py
	python3 tools/uicheck.py --presentation
	python3 tools/uicheck.py --topopt
	python3 tools/uicheck.py --lab

run: $(APP)
	./$(APP)

clean:
	rm -rf build/obj build/gen build/embed build/gltest build/lbmbench build/geomtest build/vistest $(TESTS) $(APP) $(BUNDLE) $(AM_BINS)

-include $(OBJ:.o=.d) $(CORE_OBJ:.o=.d)

# Native result-view contract tests (synthetic fields, no solver or GL context needed).
build/femviewtest: tools/femviewtest.c src/fembridge.c $(filter-out build/obj/main.o build/obj/fembridge.o,$(GUI_OBJ))
	$(CC) $(LINK_CFLAGS) tools/femviewtest.c $(filter-out build/obj/main.o build/obj/fembridge.o,$(GUI_OBJ)) -o $@ $(LDFLAGS)

# the quality gate: compiler warnings, sanitizers, the test suite, the map, operation coverage, path and history
# hygiene. One command, a table at the end, non-zero exit when something is wrong. See tools/check.sh.
.PHONY: check
check:
	@bash tools/check.sh

# the challenge leaderboards: challenges/*/LEADERBOARD.md and docs/leaderboard/index.html from each challenge's
# result files, and the agent leaderboard.
.PHONY: leaderboard
leaderboard:
	@python3 tools/leaderboard.py
	@python3 tools/agentboard.py

# the materials library: a source on every value a record claims
.PHONY: matcheck
matcheck:
	@python3 tools/matcheck.py

.PHONY: test-plugin test-printing
test-plugin: $(AM_BINS)
	python3 tools/mcptest.py
	python3 tools/mcp_http_test.py

test-printing: build/mechtest build/meshtest $(AM_BINS)
	./build/mechtest --printing
	./build/meshtest
	python3 tools/printflow.py
	python3 tools/lpbfflow.py
