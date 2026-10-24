/* jschema.h - JSON Schema (draft 2020-12 subset) validation with defaults
 *
 * The operation schemas served to MCP clients are the same documents used to validate requests, so the advertised
 * contract and the enforced one cannot drift apart.
 *
 * Supported: type (incl. integer and type arrays), enum, const, properties, required, additionalProperties,
 * items, minItems, maxItems, minimum, maximum, exclusiveMinimum, exclusiveMaximum, minLength, maxLength,
 * minProperties, maxProperties, allOf, anyOf, oneOf, $ref to "#/$defs/<name>", default (applied to missing
 * properties), plus annotations (title, description, examples, $comment, format, deprecated, readOnly).
 * Extensions: "x-unit" (default unit of a quantity) and "x-dimension" (a string instance must parse as a
 * quantity of that dimension, e.g. "0.2 mm"). Anything else is rejected by jschema_check. */
#pragma once

#include <stdbool.h>

#include "json.h"

enum { JSCHEMA_MAX_ERRORS = 8 };

typedef struct JsonSchemaIssue {
    char path[192];    /* JSON pointer into the instance, e.g. /query/all/0/max_angle_deg */
    char message[320];
} JsonSchemaIssue;

typedef struct JsonSchemaReport {
    int count;
    JsonSchemaIssue issues[JSCHEMA_MAX_ERRORS];
} JsonSchemaReport;

/* root holds "$defs". When apply_defaults, missing properties that declare a default receive a copy of it. */
bool jschema_validate(const JsonValue *schema, const JsonValue *root, JsonValue *instance, bool apply_defaults,
                      JsonSchemaReport *report);
/* Static check of a schema: unknown keywords, dangling $ref, required names missing from properties,
 * defaults that do not satisfy their own schema. */
bool jschema_check(const JsonValue *schema, const JsonValue *root, char *err, size_t errlen);
/* One-line summary of the report ("/units: must be one of ..."; several issues joined with "; "). */
void jschema_report_text(const JsonSchemaReport *r, char *out, size_t cap);
