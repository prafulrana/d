#include "bake_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <libavutil/hwcontext_cuda.h>

#include "pipeline_stages/00_common.inc.c"
#include "pipeline_stages/10_finalize_encode_progress.inc.c"
#include "pipeline_stages/20_source_model_motion.inc.c"
#include "pipeline_stages/30_manifest_frame.inc.c"
