# src/mech/mech.mk - build fragment of the mechanics layer, included by the top-level Makefile directly after CORE_OBJ.
# It appends the mechanics sources to the headless core (so every server, test and the app link them) and adds its own
# verification binary and test target without editing the shared recipes.
.DEFAULT_GOAL := all

MECH_SRC := $(wildcard src/mech/*.c)
CORE_SRC += $(MECH_SRC)
CORE_OBJ += $(patsubst src/%.c,build/obj/%.o,$(MECH_SRC)) build/obj/gen/mech_ops_schema.o
MECH_TESTS := build/mechtest build/dyntest

# the mechanics operation contract is embedded like the core schema and merged into the registry at start
build/gen/mech_ops_schema.c: src/mech/mech_ops_schema.json build/embed
	@mkdir -p build/gen
	./build/embed $< $@ NAVIER_MECH_OPS_SCHEMA

build/obj/gen/mech_ops_schema.o: build/gen/mech_ops_schema.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c $< -o $@

build/mechtest: tools/mechtest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

# the dynamics suite (T18 phase B): large deformation, explicit dynamics, contact
build/dyntest: tools/dyntest.c $(CORE_OBJ)
	$(CC) $(LINK_CFLAGS) $^ -o $@ $(CORE_LDLIBS)

.PHONY: test-mech clean-mech

test-mech: $(MECH_TESTS) navier-mcp
	./build/mechtest
	./build/dyntest
	python3 tools/mechflow.py
	python3 tools/printflow.py
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
