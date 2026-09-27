/* mech_ops.h - mechanics operations served through the shared operation registry (handlers in mech_ops.c) */
#pragma once

#include "../core/json.h"

/* adds the "mechanics" section (fidelity levels with their status, assumptions and exclusions) to capabilities_get */
void mech_capabilities_json(JsonValue *capabilities);
