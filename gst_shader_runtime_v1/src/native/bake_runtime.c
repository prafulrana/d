/* d native runtime implementation.
 * Keep this file as the ordered manifest of runtime modules; implementation
 * lives under src/native/runtime so graph/schema/runtime changes stay local.
 */
#include "runtime/00_prelude.inc.c"
#include "runtime/10_maxine_vsr.inc.c"
#include "runtime/20_nvof.inc.c"
#include "runtime/30_worker_lifecycle.inc.c"
#include "runtime/40_trt_upscalers.inc.c"
#include "runtime/50_tuning_clock.inc.c"
#include "runtime/60_input_open.inc.c"
#include "runtime/70_maxine_audio.inc.c"
#include "runtime/80_output_context.inc.c"
#include "runtime/90_audio_encode.inc.c"
#include "runtime/91_audio_process.inc.c"
#include "runtime/92_request_state.inc.c"
#include "runtime/92_graph_cadence.inc.c"
#include "runtime/92_receiver_clock.inc.c"
#include "runtime/92_stage_clock.inc.c"
#include "runtime/92_perf_ring.inc.c"
#include "runtime/92_timing_evidence.inc.c"
#include "runtime/92_intermediate_motion.inc.c"
#include "runtime/93_run_loop.inc.c"
#include "runtime/94_public_run.inc.c"
