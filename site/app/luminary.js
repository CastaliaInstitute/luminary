// Luminary PWA: drives the WebAssembly build of the shared C core (the same
// renderer as the Android app and P4 firmware) into a canvas, tracks the real
// day with an on-device solar clock, and polls the same-origin runtime bundle
// for live buoy waves and GOES clouds.
import { sunPosition } from './solar.js';

const RUNTIME = '/runtime/v1';                 // same Pages origin as this app
const ASSET = (n) => `./assets/${n}`;
const statusEl = () => document.getElementById('status');
const setStatus = (t) => { const e = statusEl(); if (e) e.textContent = t; };

let Mod, W, H, fbPtr, ctx, activeScene = {}, startTime = 0;

async function fetchBin(url) {
  const r = await fetch(url, { cache: 'no-store' });
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

// Decode the base JPEG to packed RGB (the core wants 3 bytes/pixel).
async function fetchBaseRgb(w, h) {
  const blob = await (await fetch(ASSET('nubble_runtime_base.jpg'))).blob();
  const bmp = await createImageBitmap(blob);
  const c = new OffscreenCanvas(w, h);
  const cx = c.getContext('2d');
  cx.drawImage(bmp, 0, 0, w, h);
  const rgba = cx.getImageData(0, 0, w, h).data;
  const rgb = new Uint8Array(w * h * 3);
  for (let i = 0, j = 0; i < rgba.length; i += 4, j += 3) {
    rgb[j] = rgba[i]; rgb[j + 1] = rgba[i + 1]; rgb[j + 2] = rgba[i + 2];
  }
  return rgb;
}

// Copy bytes into wasm memory; returns the pointer (caller owns it).
function toWasm(u8) {
  const p = Mod._malloc(u8.length);
  Mod.HEAPU8.set(u8, p);
  return p;
}

async function init() {
  setStatus('loading engine…');
  Mod = await LuminaryModule();
  W = Mod._lum_wasm_width();
  H = Mod._lum_wasm_height();

  setStatus('loading scene…');
  const [base, water, land, shore, map, depth, cl, cm, ch] = await Promise.all([
    fetchBaseRgb(W, H),
    fetchBin(ASSET('nubble_runtime_water_mask.bin')),
    fetchBin(ASSET('nubble_runtime_land_mask.bin')),
    fetchBin(ASSET('nubble_runtime_shore_distance.bin')),
    fetchBin(ASSET('nubble_runtime_ocean_map.bin')),
    fetchBin(ASSET('nubble_runtime_ocean_depth.bin')),
    fetchBin(ASSET('nubble_runtime_cloud_low.bin')),
    fetchBin(ASSET('nubble_runtime_cloud_mid.bin')),
    fetchBin(ASSET('nubble_runtime_cloud_high.bin')),
  ]);
  // Persistent assets (core reads in place); initial clouds can be freed after.
  const ok = Mod._lum_wasm_init(
    toWasm(base), toWasm(water), toWasm(land), toWasm(shore),
    toWasm(map), toWasm(depth), toWasm(cl), toWasm(cm), toWasm(ch),
  );
  if (!ok) { setStatus('engine init failed'); return; }
  fbPtr = Mod._lum_wasm_framebuffer();

  const canvas = document.getElementById('scene');
  canvas.width = W; canvas.height = H;
  ctx = canvas.getContext('2d');

  applyScene(activeScene);                       // bundled defaults until fetched
  startTime = performance.now();
  requestAnimationFrame(loop);

  // Solar clock: re-apply every second so the sky advances with the real day.
  setInterval(() => applyScene(activeScene), 1000);
  pollLive();                                    // first fetch now…
  setInterval(pollLive, 10 * 60 * 1000);         // …then every 10 minutes
  setStatus('');
  document.body.classList.add('running');
}

// Translate the runtime scene JSON to core conditions (mirrors the Android
// LuminaryCore.applyScene). The sun is always computed on-device.
function applyScene(scene) {
  activeScene = scene || {};
  const sky = activeScene.sky_color || activeScene.sky || {};
  const clouds = activeScene.clouds || {};
  const ocean = activeScene.ocean || {};

  const waves = [];
  const comps = ocean.components;
  if (Array.isArray(comps)) {
    for (const c of comps.slice(0, 3)) {
      waves.push(Math.round((c.height_m ?? 0.5) * 1000));
      waves.push(Math.round((c.period_s ?? 7.0) * 1000));
      waves.push(Math.round(c.wave_from_deg ?? ocean.wave_from_deg ?? 137));
    }
  }
  if (waves.length === 0) waves.push(500, 7000, 137);
  const waveCount = waves.length / 3;

  const shellDefaults = [[6000, 14], [3000, 9], [1200, 4]];
  const shells = [];
  const arr = clouds.shells;
  for (let s = 0; s < 3; s++) {
    const sh = Array.isArray(arr) ? arr[s] : null;
    shells.push(Math.round((sh?.wind_east_mps ?? 4.0) * 1000));
    shells.push(Math.round((sh?.wind_north_mps ?? 2.0) * 1000));
    shells.push(Math.round(sh?.height_m ?? shellDefaults[s][0]));
    shells.push(shellDefaults[s][1]);
  }

  const sun = sunPosition();
  const altDeci = Math.round(sun.altitudeDeg * 10);
  const sunMode = altDeci >= 0 ? 0 : altDeci >= -60 ? 1 : altDeci >= -120 ? 2 : 3;
  const cover = Math.round(
    (clouds.cover_fraction ?? activeScene.cloud_cover ?? 0) * 1000);

  const wPtr = Mod._malloc(waves.length * 4);
  const sPtr = Mod._malloc(shells.length * 4);
  Mod.HEAPU32.set(Int32Array.from(waves), wPtr >> 2);
  Mod.HEAPU32.set(Int32Array.from(shells), sPtr >> 2);
  Mod._lum_wasm_set_conditions(
    sky.r ?? 168, sky.g ?? 208, sky.b ?? 228,
    sunMode, altDeci, Math.round((sun.azimuthDeg - 90) * 10),
    cover, waveCount, wPtr, sPtr,
  );
  Mod._free(wPtr); Mod._free(sPtr);
}

let acc = 0, last = 0;
function loop(now) {
  if (!last) last = now;
  acc += now - last; last = now;
  // 30 Hz solver, capped so a stall doesn't spiral.
  let ticks = 0;
  while (acc >= 33 && ticks < 4) { Mod._lum_wasm_tick(); acc -= 33; ticks++; }
  Mod._lum_wasm_render(now - startTime);
  // The framebuffer bytes are r,g,b,255 -- already ImageData layout. Re-wrap the
  // heap each frame in case a fetch grew (and moved) wasm memory.
  const view = new Uint8ClampedArray(Mod.HEAPU8.buffer, fbPtr, W * H * 4);
  ctx.putImageData(new ImageData(view, W, H), 0, 0);
  requestAnimationFrame(loop);
}

// Poll the same-origin runtime bundle: apply live state, hot-swap live clouds.
async function pollLive() {
  try {
    const man = await (await fetch(`${RUNTIME}/manifest.json`, { cache: 'no-store' })).json();
    const path = (k) => man.assets?.[k]?.path || man.assets?.[k]?.file;
    if (path('state')) {
      const scene = await (await fetch(`${RUNTIME}/${path('state')}`, { cache: 'no-store' })).json();
      applyScene(scene);
    }
    const need = Mod._lum_wasm_cloud_bytes();
    const lo = path('cloud_low'), mi = path('cloud_mid'), hi = path('cloud_high');
    if (lo && mi && hi) {
      const [a, b, c] = await Promise.all([
        fetchBin(`${RUNTIME}/${lo}`), fetchBin(`${RUNTIME}/${mi}`), fetchBin(`${RUNTIME}/${hi}`),
      ]);
      if (a.length === need && b.length === need && c.length === need) {
        const pa = toWasm(a), pb = toWasm(b), pc = toWasm(c);
        Mod._lum_wasm_set_clouds(pa, pb, pc);    // low, mid, high
        Mod._free(pa); Mod._free(pb); Mod._free(pc);
        console.info(`live GOES clouds applied (${a.length} B/shell)`);
      } else {
        console.warn(`live cloud atlas ${a.length} B != ${need}; keeping bundled`);
      }
    }
  } catch (e) {
    console.warn('live fetch failed; keeping current conditions', e);
  }
}

// Register the offline service worker (best-effort).
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('./sw.js').catch(() => {});
}

init().catch((e) => { console.error(e); setStatus('failed: ' + e.message); });
