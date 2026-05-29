#include "pipeline_manifest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "generated/d_pipeline_contract.h"

#include "pipeline_manifest.d/00_json_helpers.inc.c"
#include "pipeline_manifest.d/10_stage_parse.inc.c"
#include "pipeline_manifest.d/20_manifest_api.inc.c"
