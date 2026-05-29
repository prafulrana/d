const DEFAULT_CONTROL_URL = 'http://127.0.0.1:8088';
const DEFAULT_PROGRAM = 'default';

function cleanBaseUrl(value) {
  return String(value || DEFAULT_CONTROL_URL).replace(/\/+$/u, '');
}

function programPath(programName) {
  return encodeURIComponent(String(programName || DEFAULT_PROGRAM));
}

async function jsonResponse(response) {
  const body = await response.json().catch(async () => ({ raw: await response.text().catch(() => '') }));
  if (!response.ok || body?.ok === false) {
    const error = new Error(body?.error || `d_http_${response.status}`);
    error.status = response.status;
    error.body = body;
    throw error;
  }
  return body;
}

export const DEFAULT_VIDEO_STAGES = Object.freeze([
  { id: 'decode_nv12', kind: 'nvdec', plugin: 'd-native-decode' },
  { id: 'nv12_to_rgb_chw', kind: 'pre', plugin: 'd-native-video' },
  { id: 'nvof_fruc', kind: 'nvof', plugin: 'd-native-video' },
  { id: 'upscaler', kind: 'upscaler', plugin: 'd-native-video' },
  { id: 'rgb_chw_to_rgba8', kind: 'pre', plugin: 'd-native-video' },
  { id: 'post_vsr_finalize', kind: 'cuda', plugin: 'd-native-video' },
  { id: 'deband_4k', kind: 'cuda', plugin: 'd-native-video' },
  { id: 'custom_shader', kind: 'cuda', plugin: 'd-native-video' },
  { id: 'dlsaa_temporal', kind: 'cuda', plugin: 'd-native-video' },
  { id: 'temporal_denoise', kind: 'temporal', plugin: 'd-native-video' },
  { id: 'nvenc_hevc', kind: 'nvenc-hevc', plugin: 'd-native-encode' },
]);

export const DEFAULT_AUDIO_STAGES = Object.freeze([
  { id: 'audio_decode', kind: 'audio_decode', plugin: 'd-native-audio' },
  { id: 'audio_to_16k_mono', kind: 'audio_resample', plugin: 'd-native-audio' },
  { id: 'maxine_audio_cleanup', kind: 'audio_maxine', plugin: 'd-native-audio' },
  { id: 'maxine_audio_superres', kind: 'audio_maxine', plugin: 'd-native-audio' },
  { id: 'audio_eq_profile', kind: 'audio_post', plugin: 'd-native-audio' },
  { id: 'audio_delay_sync', kind: 'audio_sync', plugin: 'd-native-audio' },
  { id: 'audio_to_stereo_48k', kind: 'audio_post', plugin: 'd-native-audio' },
  { id: 'audio_aac_transport', kind: 'audio_encode', plugin: 'd-native-audio' },
]);

export const DEFAULT_CLOCK_POLICY = Object.freeze({
  kind: 'live',
  videoClockMode: 'source-pts',
  audioPacingMode: 'video-gated',
  maxAudioLeadMs: 750,
  maxAvDeltaMs: 250,
});

export const DEFAULT_FILE_CLOCK_POLICY = Object.freeze({
  kind: 'vod',
  videoClockMode: 'source-pts',
  audioPacingMode: 'source-pts',
  maxAudioLeadMs: 0,
  maxAvDeltaMs: 250,
});

export function buildLiveGraph({
  sourceUri,
  sourceHeaders = '',
  sinkUri,
  runtimeName = 'd-main',
  programName = DEFAULT_PROGRAM,
  runtimeParams = {},
  clockPolicy = DEFAULT_CLOCK_POLICY,
  outputWidth = 3840,
  outputHeight = 2160,
  outputFps = 60,
  processingWidth = 1280,
  processingHeight = 720,
  decodeFpsCap = 0,
  bitrateBps = 24_000_000,
  maxBitrateBps = 32_000_000,
  perfRingPath = '',
  dPipeline,
  stages = DEFAULT_VIDEO_STAGES,
  audioStages = DEFAULT_AUDIO_STAGES,
}) {
  if (!sourceUri) throw new Error('d_graph_source_uri_required');
  if (!sinkUri) throw new Error('d_graph_sink_uri_required');
  if (!dPipeline || typeof dPipeline !== 'object') throw new Error('d_graph_d_pipeline_required');
  return {
    $schema: './d-pipeline.schema.json',
    runtimeName,
    programName,
    runtimeParams,
    sourceUri,
    sourceHeaders,
    isLive: true,
    executionMode: 'live',
    clockPolicy,
    outputWidth,
    outputHeight,
    outputFps,
    processingWidth,
    processingHeight,
    decodeFpsCap,
    bitrateBps,
    maxBitrateBps,
    perfRingPath,
    sinkUri,
    dPipeline,
    stages: [...stages],
    audioStages: [...audioStages],
  };
}

export function buildFileGraph({
  sourceUri,
  outputUri = '',
  sinkUri = outputUri,
  ...rest
}) {
  if (!sourceUri) throw new Error('d_graph_source_uri_required');
  if (!sinkUri) throw new Error('d_graph_sink_or_output_uri_required');
  return {
    ...buildLiveGraph({ sourceUri, sinkUri, ...rest }),
    isLive: false,
    executionMode: 'file',
    clockPolicy: rest.clockPolicy || DEFAULT_FILE_CLOCK_POLICY,
    outputUri,
  };
}

export class DRuntimeClient {
  constructor({
    controlUrl = DEFAULT_CONTROL_URL,
    programName = DEFAULT_PROGRAM,
    fetchImpl = globalThis.fetch,
  } = {}) {
    if (!fetchImpl) throw new Error('fetch_impl_required');
    this.controlUrl = cleanBaseUrl(controlUrl);
    this.programName = programName;
    this.fetch = fetchImpl;
  }

  programUrl(programName = this.programName) {
    return `${this.controlUrl}/v1/programs/${programPath(programName)}`;
  }

  async status({ timeoutMs = 5000 } = {}) {
    const response = await this.fetch(`${this.controlUrl}/v1/status`, {
      headers: { accept: 'application/json' },
      signal: AbortSignal.timeout(timeoutMs),
    });
    return jsonResponse(response);
  }

  async select(graph, { programName = this.programName, timeoutMs = 15000 } = {}) {
    const response = await this.fetch(`${this.programUrl(programName)}/select`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(graph),
      signal: AbortSignal.timeout(timeoutMs),
    });
    return jsonResponse(response);
  }

  async stop({ programName = this.programName, timeoutMs = 5000 } = {}) {
    const response = await this.fetch(`${this.programUrl(programName)}/stop`, {
      method: 'POST',
      signal: AbortSignal.timeout(timeoutMs),
    });
    return jsonResponse(response);
  }
}
