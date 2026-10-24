# OpenPhysicsAI - the macOS app (with the real-time 3D lattice Boltzmann water tunnel), the engineering core and the lab
CC       := clang
UNAME    := $(shell uname -s)
# tuned for this machine's processor: -mcpu=native where clang takes it (Apple silicon, arm64), else -march=native
# (x86-64, where clang refuses -mcpu). PORTABLE=1 builds for the architecture's baseline instead, for release archives
# that must run on other machines. -ffp-contract=off below keeps fused multiply-adds out of the results either way.
ifeq ($(PORTABLE),1)
CPUFLAG  :=
else
CPUFLAG  := $(shell if $(CC) -mcpu=native -E -x c /dev/null >/dev/null 2>&1; then echo -mcpu=native; \
                    elif $(CC) -march=native -E -x c /dev/null >/dev/null 2>&1; then echo -march=native; fi)
endif
CFLAGS   := -std=c11 -O3 $(CPUFLAG) -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -pthread -MMD -MP
# code that must keep exact IEEE NaN/Inf semantics (file parsing, tests) is built without -ffast-math
NOFAST   := $(filter-out -ffast-math,$(CFLAGS))
# OpenPhysicsAI numerical core, control layer, servers and tests: IEEE semantics (no -ffast-math, so NaN checks and summation
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
APP      := openphysics
BUNDLE   := OpenPhysicsAI.app

# AM core: no OpenGL, Cocoa or UI state, so it also builds headless on Linux
CORE_SRC := $(wildcard src/core/*.c) $(wildcard src/ctl/*.c) $(wildcard src/net/*.c) $(wildcard src/fem/*.c) $(wildcard src/render/*.c) src/geom/mesh.c src/geom/bvh.c src/geom/surface.c src/geom/patches.c src/geom/hexmesh.c src/geom/repair.c src/geom/tetmesh.c
CORE_OBJ := $(patsubst src/%.c,build/obj/%.o,$(CORE_SRC)) build/obj/gen/ops_schema.o build/obj/gen/materials_data.o build/obj/common.o build/obj/threads.o build/obj/headless/lbm.o
-include src/mech/mech.mk
-include src/lab/lab.mk
-include src/cad/cad.mk
# the lattice Boltzmann kernel is shared: the GUI links its own fast-math build (build/obj/lbm.o), the headless core an
# IEEE build of the same source (build/obj/headless/lbm.o), so the two never meet in one binary
GUI_OBJ := $(sort $(OBJ) $(filter-out build/obj/headless/lbm.o,$(CORE_OBJ)))
LINK_CFLAGS := $(filter-out -MMD -MP,$(CORE_CFLAGS))
AM_BINS  := openphysics-server openphysics-mcp openphysics-ctl
# the names before the rename of 2026-10-21, for one release: links to the new programs, so that an MCP client
# configuration that names navier-mcp, or a script that runs ./navier-ctl, keeps working (make clean removes them)
AM_ALIASES := navier-server navier-mcp navier-ctl
TESTS    := build/coretest build/surftest build/meshtest build/femtest build/rendertest build/opstest build/ctltest build/amtest build/thermtest build/tsteptest build/orchtest build/advtest build/evaltest build/tettest build/topotest

.PHONY: xr xr-bridge all app clean tools run test am headless
ifeq ($(UNAME),Darwin)
# CAD (build/openphysics-cad) needs Open CASCADE and Manifold, about an hour and 1.7 GB to build once per machine, so
# `make` builds it only when those libraries are already there; `make cad build/openphysics-cad` builds them first
CAD_READY := $(if $(wildcard $(OCCT_DIR)/.openphysics-built $(OCCT_DIR)/.navier-built),build/openphysics-cad build/navier-cad)
all: $(APP) $(AM_BINS) navier $(AM_ALIASES) $(CAD_READY)
else
# the app needs Cocoa, OpenGL and Core Text: elsewhere `make` is the headless build, so `make && make lab` works anywhere
all: headless
	@echo "make: the app (./openphysics) is macOS only; this built the headless targets (servers, tests, the lab's tools)"
endif

am: $(AM_BINS) $(AM_ALIASES)

# everything that builds without Cocoa/OpenGL (the Linux headless target)
headless: $(AM_BINS) $(AM_ALIASES) $(TESTS)

$(AM_ALIASES): navier-%: openphysics-%
	ln -sf $< $@
navier: $(APP)
	ln -sf $(APP) $@
build/navier-cad: build/openphysics-cad
	ln -sf openphysics-cad $@

openphysics-server: src/bin/openphysics_server.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

openphysics-mcp: src/bin/openphysics_mcp.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

openphysics-ctl: src/bin/openphysics_ctl.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

$(APP): $(GUI_OBJ)
	$(CC) $(GUI_OBJ) -o $@ $(LDFLAGS)

# the commit the app is built from, for the run records Agent mode writes; the header is rewritten only when it changes
build/obj/app.o: build/gen/app_commit.h
build/gen/app_commit.h: FORCE
	@mkdir -p $(dir $@); c=$$(git rev-parse --short HEAD 2>/dev/null || echo unknown); \
	git diff --quiet HEAD -- src 2>/dev/null || c="$$c with local changes"; \
	echo "#define OPENPHYSICS_APP_COMMIT \"$$c\"" > $@.tmp; cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
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

# (written again on every run and replaced only when it changed, so that a checkout built before the rename of
# 2026-10-21 gets the symbols' new names)
build/gen/ops_schema.c: src/ctl/ops_schema.json build/embed FORCE
	@mkdir -p build/gen
	@./build/embed $< $@.tmp OPENPHYSICS_OPS_SCHEMA && { cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@; }

build/obj/gen/ops_schema.o: build/gen/ops_schema.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c $< -o $@

# the material library (demonstration values, labelled as such) is embedded the same way
build/gen/materials_data.c: src/ctl/materials.json build/embed FORCE
	@mkdir -p build/gen
	@./build/embed $< $@.tmp OPENPHYSICS_MATERIALS && { cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@; }

build/obj/gen/materials_data.o: build/gen/materials_data.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c $< -o $@

app: $(APP) openphysics-mcp build/labrun
	@mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	cp $(APP) $(BUNDLE)/Contents/MacOS/openphysics
	cp tools/Info.plist $(BUNDLE)/Contents/Info.plist
	@# Agent mode points the user's AI tool at openphysics-mcp beside the app; Simple mode opens the samples and presets
	@if [ -x openphysics-mcp ]; then cp openphysics-mcp $(BUNDLE)/Contents/MacOS/openphysics-mcp; fi
	@# the microphone of the agentic bar, when it is built (make voice)
	@if [ -x build/openphysics-mic ]; then cp build/openphysics-mic $(BUNDLE)/Contents/MacOS/openphysics-mic; fi
	@mkdir -p $(BUNDLE)/Contents/Resources/samples
	cp samples/*.stl samples/samples.json $(BUNDLE)/Contents/Resources/samples/
	cp src/ctl/print_profiles.json $(BUNDLE)/Contents/Resources/print_profiles.json
	@mkdir -p $(BUNDLE)/Contents/Resources/examples/lab $(BUNDLE)/Contents/Resources/docs/lab $(BUNDLE)/Contents/Resources/build
	cp examples/lab/*.json $(BUNDLE)/Contents/Resources/examples/lab/
	cp docs/lab/*.md $(BUNDLE)/Contents/Resources/docs/lab/
	cp build/labrun $(BUNDLE)/Contents/Resources/build/labrun
	@echo "built $(BUNDLE) - double-click it in Finder, files are saved to ~/Documents/OpenPhysicsAI"

build/gltest: tools/gltest.c build/obj/platform_macos.o build/obj/glutil.o build/obj/image.o build/obj/font.o build/obj/common.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

build/lbmbench: tools/lbmbench.c build/obj/lbm.o build/obj/common.o build/obj/threads.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

build/geomtest: tools/geomtest.c build/obj/geom/mesh.o build/obj/geom/shapes.o build/obj/geom/voxel.o build/obj/common.o build/obj/threads.o
	$(CC) $(NOFAST) $^ -o $@ $(LDFLAGS)

build/vistest: tools/vistest.c build/obj/vis_fields.o build/obj/vis_stream.o build/obj/vis_iso.o build/obj/common.o build/obj/threads.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

tools: build/gltest build/lbmbench build/geomtest build/vistest

# ---- OpenPhysicsAI tests (headless) ----
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

# generative design with the topology optimiser: the titanium engine-mount bracket of the film (tools/render/README.md)
build/gendesign: tools/gendesign.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

# headless test suite: unit tests, operation layer, control socket, MCP protocol and the two complete AI-driven
# workflows (structural: amflow; thermal, anisotropy and melting: thermflow), both with independent Python clients
test: $(TESTS) $(AM_BINS) $(NOZZLE3D_TEST) build/euler3dtest
	./build/coretest
	./build/surftest
	$(if $(NOZZLE3D_TEST),./build/nozzle3dtest)
	./build/euler3dtest --sod-only
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
	python3 tools/ringloads.py --quick
	./openphysics-ctl doctor

# the tier a session runs after each step (AGENTS.md rule 8): C verification plus MCP transport checks, no physics
# flow scripts or doctor. Long reactive-plume, room steady-state and FSI statistical cases retain their complete
# coverage in test-lab-validation; explicit session subsets below check conservative laws with unchanged thresholds.
# `make test` runs the engineering workflow tier detached before a wave's final report and in CI.
.PHONY: test-fast
test-fast: $(TESTS) $(MECH_TESTS) $(LAB_TESTS) $(AM_BINS)
	./build/coretest
	./build/surftest
	$(if $(NOZZLE3D_TEST),./build/nozzle3dtest)
	./build/euler3dtest --sod-only
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
	./build/mechtest
	./build/dyntest
	./build/poissontest
	./build/firetest --fast
	./build/roomtest D
	./build/fsitest --fast
	./build/battest
	./build/melttest
	./build/sheettest
	./build/peritest
	./build/euler3dtest
	./build/labtest
	./build/labscenetest
	./build/labxrtest
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
	./build/geartest --quick
	python3 tools/mcptest.py
	python3 tools/mcp_http_test.py

# Complete original fire, room and FSI validations. Fire alone is about 25 minutes on this fanless shared laptop;
# launch detached, for example: nohup make test-lab-validation > /tmp/openphysics-lab-validation.log 2>&1 &
# The session subsets above do not establish these steady-state/statistical validation results.
.PHONY: test-lab-validation
test-lab-validation: build/firetest build/roomtest build/fsitest
	./build/firetest
	./build/roomtest
	./build/fsitest

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

# what the build made; build/third_party (MFEM, built once per clone) and the tracked $(BUNDLE)/Contents/Info.plist stay
clean:
	rm -rf build/obj build/gen build/embed build/gltest build/lbmbench build/geomtest build/vistest $(TESTS) $(APP) $(AM_BINS)
	rm -rf $(LAB_TESTS) $(LAB3D_TESTS) $(LAB_TOOLS) build/femviewtest build/labapptest build/openphysics-cad build/cadtest \
	  build/cellstifftest build/specimentest build/openphysics-mic build/openphysics-listen build/openphysics-voice \
	  build/OpenPhysicsAIListen.app build/rendertest_out
	rm -rf $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	rm -f navier $(AM_ALIASES) build/navier-cad

-include $(OBJ:.o=.d) $(CORE_OBJ:.o=.d)

# Native result-view contract tests (synthetic fields, no solver or GL context needed).
build/femviewtest: tools/femviewtest.c src/fembridge.c $(filter-out build/obj/main.o build/obj/fembridge.o,$(GUI_OBJ))
	$(CC) $(LINK_CFLAGS) tools/femviewtest.c $(filter-out build/obj/main.o build/obj/fembridge.o,$(GUI_OBJ)) -o $@ $(LDFLAGS)

# Scenario preparation, background completion and saved-result contracts; no physical solve or GL context.
build/labapptest: tools/labapptest.c src/labapp.c src/labapp.h $(filter-out build/obj/main.o build/obj/labapp.o,$(GUI_OBJ))
	$(CC) $(LINK_CFLAGS) tools/labapptest.c $(filter-out build/obj/main.o build/obj/labapp.o,$(GUI_OBJ)) -o $@ $(LDFLAGS)

.PHONY: test-labapp
# the headset link (docs/xr.md): the speech helper uses the Mac's own speech recognition (macOS only)
build/openphysics-listen: tools/xr/listen.swift tools/xr/listen-Info.plist
	@mkdir -p build
	swiftc -O tools/xr/listen.swift -o $@ -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker tools/xr/listen-Info.plist

# the microphone of the agentic bar (GOALS.md G18): the Mac's own speech recognition (macOS only)
# the same recogniser as a background app with its own name: macOS asks once whether "OpenPhysicsAI Listen" may recognise
# speech, whoever started the headset bridge (a program run from Terminal is refused: Terminal declares no use of speech)
build/OpenPhysicsAIListen.app/Contents/MacOS/listen: tools/xr/listen.swift tools/xr/listen-app-Info.plist
	@mkdir -p build/OpenPhysicsAIListen.app/Contents/MacOS
	cp tools/xr/listen-app-Info.plist build/OpenPhysicsAIListen.app/Contents/Info.plist
	swiftc -O tools/xr/listen.swift -o $@
	codesign --force --sign - build/OpenPhysicsAIListen.app

build/openphysics-mic: tools/voice/mic.swift tools/voice/mic-Info.plist
	@mkdir -p build
	swiftc -O tools/voice/mic.swift -o $@ -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker tools/voice/mic-Info.plist

.PHONY: voice
voice: build/openphysics-mic

build/openphysics-voice: tools/xr/voice.swift
	@mkdir -p build
	swiftc -O tools/xr/voice.swift -o $@

xr: $(APP) build/openphysics-listen build/OpenPhysicsAIListen.app/Contents/MacOS/listen build/openphysics-voice build/labxr

xr-bridge: xr
	python3 tools/xr/xr_bridge.py

# the headset link end to end without a headset: its own headless app and bridges, the page in headless Chrome
.PHONY: test-xr
test-xr: $(APP) openphysics-mcp build/labxr build/labxrtest build/openphysics-voice build/labrun
	./build/labxrtest
	python3 tools/xr/change_test.py
	python3 tools/xr/talk_test.py
	python3 tools/xr/xr_test.py

test-labapp: build/labapptest
	./build/labapptest

# the first check after a clone, on macOS or Linux: it builds what it needs, then doctor, one small lab scenario, one
# picture and an MCP handshake, PASS or FAIL for each, a minute or two on a laptop (tools/smoke.sh)
.PHONY: smoke
smoke:
	@bash tools/smoke.sh "$(MAKE)"

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
	python3 tools/printskin.py --quick
	python3 tools/lpbfflow.py
