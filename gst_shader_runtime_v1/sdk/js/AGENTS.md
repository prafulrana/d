# D Runtime JS SDK Agent Canon

Tiny control-plane helper for apps that want to drive D without owning media
bytes.

```js
import { readFile } from 'node:fs/promises';
import { DRuntimeClient, buildFileGraph, buildLiveGraph } from './d-runtime-client.mjs';

const sample = JSON.parse(await readFile(new URL('../../graphs/default_video.json', import.meta.url), 'utf8'));
const dPipeline = sample.dPipeline;

const d = new DRuntimeClient({
  controlUrl: 'http://127.0.0.1:8088',
  programName: '99sk',
});

const graph = buildLiveGraph({
  sourceUri: 'https://example.invalid/live/index.m3u8',
  sourceHeaders: 'Referer: Kodi (Android)\r\n',
  sinkUri: 'unix:/run/99ks/99sk.ts.sock',
  perfRingPath: '/run/99ks/perf/example.ring',
  processingWidth: 1280,
  processingHeight: 720,
  outputFps: 60,
  dPipeline,
});

await d.select(graph);
console.log(await d.status());

const fileGraph = buildFileGraph({
  sourceUri: 'file:///inputs/source.mkv',
  outputUri: 'file:///outputs/source.99ks.ts',
  sinkUri: 'file:///outputs/source.99ks.ts',
  outputFps: 60,
  dPipeline,
});
```

Contract:

- `select(graph)` posts to `POST /v1/programs/:program/select`.
- `status()` reads `GET /v1/status`.
- `stop()` posts to `POST /v1/programs/:program/stop`.
- The SDK never opens media URLs and never forwards stream bytes.
- Live graphs publish to `sinkUri`.
- File graphs use the same canonical `dPipeline` with `isLive: false`,
  `executionMode: "file"`, and a static video `sourceUri`.
- File sinks receive encoded packets only. Do not add a CPU/ffmpeg side path.
