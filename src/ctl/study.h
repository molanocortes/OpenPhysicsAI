/* study.h - comparison studies: several designs of one part analysed under physically equivalent conditions
 *
 * A study definition (the study_check / study_run input, see ops_schema.json $defs/comparison_study) states the engineering
 * question, the designs (geometry file, unit, mounting region, load region), the material and manufacturing information,
 * the load (a payload mass with its gravitational acceleration, or a force), the mounting idealisation, the mesh
 * refinement schedule, the declared sensitivities and the resource limits. Every value carries its source: user,
 * measured, database, inferred or default.
 *
 * Resolution never guesses. Missing information that changes the answer becomes a question (blocking: the study cannot
 * run until it is answered or explicitly accepted with a reason); defaults that do not change the ranking become recorded
 * assumptions with the way their effect is assessed. The resolved study is hashed and written once as study.json.
 *
 * Execution composes the ordinary operations on private engines, one per design, so every step is an operation that an
 * AI client could also run and inspect: each design becomes a normal project under designs/<name>, meshed and solved at
 * every element size, then at the declared sensitivities. The study directory holds:
 *   request.json      the definition as submitted
 *   study.json        the resolved, immutable specification and its hash
 *   operations.jsonl  every operation the study executed, with its parameters and outcome
 *   designs/<name>/   each design's project (inputs with hashes, runs/<job>/spec.json, results.nvr, summary.json)
 *   previews/         labelled images of the mounting and load regions
 *   evidence.json     the machine-readable Engineering Evidence Record
 *   report.md         the readable record, every number traceable to evidence.json
 *
 * Coordinates in a definition are in the design frame: the STL file's axes and origin, lengths in mm. Designs are placed
 * without rotation, so the build frame differs from the design frame by a translation only (recorded per design). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "ops.h"

enum { STUDY_MAX_DESIGNS = 4, STUDY_MAX_LEVELS = 4 };

/* Resolves and checks a definition without solving (imports and meshes the coarsest level in a temporary folder).
 * Returns the report {status: ready | needs_input | not_supported, questions, assumptions, unsupported, not_evaluated,
 * warnings, resolved, study_hash, designs, equivalence, plan}; region preview images go to images. NULL with err when the
 * definition cannot even be read. */
JsonValue *study_check(Engine *e, const JsonValue *definition, OpResult *images, NvErr *code, char *err, size_t errlen);

/* Checks the definition and, when it is ready, writes the study directory and queues the study job. On success returns
 * {job_id, directory, study_hash, plan}; otherwise NULL with code/err and, for a study that needs input, *report set to
 * the check report (caller frees). */
JsonValue *study_submit(Engine *e, const JsonValue *definition, const char *directory, const char *reference_directory, JsonValue **report, NvErr *code,
                        char *err, size_t errlen);

/* SHA-256 of a resolved study without file locations: the same geometry content and conditions give the same hash wherever
 * the files lie (a replay from the stored copies has the hash of the original) */
void study_resolved_hash(const JsonValue *resolved, char out[65]);

/* evidence.json of a study directory (NULL with err when absent) */
JsonValue *study_read_evidence(const char *directory, char *err, size_t errlen);

/* the readable report of an evidence record (Markdown); caller frees */
char *study_report_markdown(const JsonValue *evidence);
