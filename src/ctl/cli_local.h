/* cli_local.h - navier-ctl without a server: operations, comparison studies and diagnostics on an engine inside the
 * command-line process */
#pragma once

#include "engine.h"

/* argv[i] is the command (call, study, doctor). Exit codes: 0 success, 1 operation error, 2 usage error, 3 job failed or
 * cancelled, 4 the study needs input, 5 not supported, 6 replay differs, 7 a diagnostic failed. */
int cli_local_main(int argc, char **argv, int i, EngineConfig *cfg);
