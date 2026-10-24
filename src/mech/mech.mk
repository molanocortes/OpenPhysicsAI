# src/mech/mech.mk - build fragment of the mechanics layer, included by the top-level Makefile directly after CORE_OBJ.
# It appends the mechanics sources to the headless core (so every server, test and the app link them) and adds its own
# verification binary and test target without editing the shared recipes.
.DEFAULT_GOAL := all

MECH_SRC := $(wildcard src/mech/*.c)
CORE_SRC += $(MECH_SRC)
CORE_OBJ += $(patsubst src/%.c,build/obj/%.o,$(MECH_SRC)) build/obj/gen/mech_ops_schema.o
MECH_TESTS := build/mechtest build/dyntest

# the mechanics operation contract is embedded like the core schema and merged into the registry at start
build/gen/mech_ops_schema.c: src/mech/mech_ops_schema.json build/embed FORCE
	@mkdir -p build/gen
	@./build/embed $< $@.tmp OPENPHYSICS_MECH_OPS_SCHEMA && { cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@; }

build/obj/gen/mech_ops_schema.o: build/gen/mech_ops_schema.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c $< -o $@

build/mechtest: tools/mechtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

build/breaktest: tools/breaktest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

# the break estimate as a library for tools/g20_strength.py (ctypes): the same C, no second implementation
build/libbreakest.dylib: src/mech/breakest.c src/mech/breakest.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -MMD -MP,$(CORE_CFLAGS)) -dynamiclib src/mech/breakest.c -o $@ -lm

# the dynamics suite (T18 phase B): large deformation, explicit dynamics, contact
build/dyntest: tools/dyntest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

.PHONY: test-mech clean-mech

test-mech: $(MECH_TESTS) openphysics-mcp
	./build/mechtest
	./build/dyntest
	python3 tools/mechflow.py
	python3 tools/printflow.py
	python3 tools/printskin.py --quick
	python3 tools/lpbfflow.py

headless: $(MECH_TESTS)
test: test-mech
clean: clean-mech
clean-mech:
	rm -f $(MECH_TESTS)

# throughput numbers for the status report (not part of the tests)
build/mechbench: tools/mechbench.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

# layer-by-layer print simulation of a part with rendered frames (demonstration tool)
build/amprint: demo/am_print.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

# G17 (GOALS.md): the strength of a printed part through the static analysis, criteria a-c of step 1
build/printstrengthtest: tools/printstrengthtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

# G17 step 0 (GOALS.md): the tier a session runs after each step of the printed-part strength work, in under 180 s:
# the result view, the mesh, the orthotropic elements and strength criteria, the materials library, the strength suite
# and the failure index through the app and the headset scene
.PHONY: test-print-fast
test-print-fast: build/femviewtest build/meshtest build/mechtest build/printstrengthtest $(APP)
	./build/femviewtest
	./build/meshtest
	./build/mechtest --orthotropic
	python3 tools/matcheck.py
	./build/printstrengthtest
	python3 tools/printstrength_app.py
