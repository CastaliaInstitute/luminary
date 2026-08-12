import { signedAngle, sunPosition } from './solar.js';

const W = 2600;
const H = 1200;
const HORIZON = 506;
const CAMERA_BEARING = 250;
const WEATHER_URL = 'https://api.weather.gov/gridpoints/GYX/65,36/forecast/hourly';
const MARINE_URL = 'https://marine-api.open-meteo.com/v1/marine?latitude=43.2207&longitude=-70.5796&current=wave_height,wave_direction,wave_period,swell_wave_height,swell_wave_direction,swell_wave_period,wind_wave_height,wind_wave_direction,wind_wave_period&timezone=America%2FNew_York';
const REFRESH_MS = 10 * 60 * 1000;
const CACHE_KEY = 'cliff-house-live-v1';
const params = new URLSearchParams(location.search);
const canvas = document.querySelector('#scene');
const ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });
const hud = document.querySelector('#hud');
const conditionsEl = document.querySelector('#conditions');
const offlineEl = document.querySelector('#offline');

let registration;
let foreground;
let oceanTexture;
let measuredCloudFrames = [];
let measuredCloudMaps = [];
let startedAt = performance.now();
let lastUpdated = null;
let weather = { cloudCover: 25, windMph: 8, windDirection: 'SW', temperatureF: null, description: 'Live sea and sky' };
let sea = { waveHeightM: 0.45, waveDirectionDeg: 145, wavePeriodS: 6, swellHeightM: 0.15, swellDirectionDeg: 135, swellPeriodS: 10 };

const hash = (v) => Math.abs(Math.sin(v * 12.9898) * 43758.5453) % 1;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const mix = (a, b, t) => a.map((v, i) => Math.round(v + (b[i] - v) * t));
const rgba = (c, a = 1) => `rgba(${c[0]},${c[1]},${c[2]},${a})`;

function image(url) {
  return new Promise((resolve, reject) => {
    const value = new Image();
    value.decoding = 'async';
    value.onload = () => resolve(value);
    value.onerror = reject;
    value.src = url;
  });
}

async function json(url) {
  const response = await fetch(url, { cache: 'no-store', headers: { Accept: 'application/json, application/geo+json' } });
  if (!response.ok) throw new Error(`${url}: ${response.status}`);
  return response.json();
}

function cloudFraction(description = '') {
  const text = description.toLowerCase();
  if (/overcast/.test(text)) return 96;
  if (/mostly cloudy/.test(text)) return 78;
  if (/partly cloudy/.test(text)) return 48;
  if (/mostly clear/.test(text)) return 18;
  if (/clear|sunny/.test(text)) return 6;
  if (/fog|rain|snow|shower/.test(text)) return 88;
  if (/cloud/.test(text)) return 62;
  return 30;
}

function cardinalDegrees(direction) {
  const points = ['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];
  const index = points.indexOf(direction);
  return index < 0 ? 225 : index * 22.5;
}

function palette(altitude) {
  const day = clamp((altitude + 8) / 24, 0, 1);
  const twilight = clamp(1 - Math.abs(altitude + 2) / 11, 0, 1);
  return {
    day,
    twilight,
    top: mix([2, 7, 16], [55, 135, 198], day),
    horizon: mix([9, 14, 24], [184, 215, 231], day),
  };
}

function drawSun(sun, colors) {
  if (sun.altitudeDeg < -1.2) return;
  const relative = signedAngle(sun.azimuthDeg - CAMERA_BEARING);
  const hfov = 110;
  if (Math.abs(relative) > hfov * 0.58) return;
  const x = W / 2 + Math.tan(relative * Math.PI / 180) / Math.tan(hfov * Math.PI / 360) * W / 2;
  const focalY = (HORIZON * 0.92) / Math.tan(28 * Math.PI / 180);
  const y = HORIZON - Math.tan(sun.altitudeDeg * Math.PI / 180) * focalY;
  const radius = 16 + 15 * (1 - colors.day);
  const glow = ctx.createRadialGradient(x, y, 0, x, y, radius * 9);
  glow.addColorStop(0, 'rgba(255,249,218,.98)');
  glow.addColorStop(.08, 'rgba(255,224,146,.86)');
  glow.addColorStop(1, 'rgba(255,157,74,0)');
  ctx.fillStyle = glow;
  ctx.beginPath(); ctx.arc(x, y, radius * 9, 0, Math.PI * 2); ctx.fill();
  ctx.fillStyle = 'rgba(255,247,220,.96)';
  ctx.beginPath(); ctx.arc(x, y, radius, 0, Math.PI * 2); ctx.fill();
}

const cloudSeeds = Array.from({ length: 52 }, (_, i) => ({
  azimuth: hash(i * 19.7) * 2 - 1,
  elevation: .08 + hash(i * 41.3) * .84,
  scale: .45 + hash(i * 73.1) * 1.3,
  shell: i % 3,
}));

function drawProceduralClouds(seconds, colors) {
  const cover = clamp(weather.cloudCover / 100, 0, 1);
  if (cover < .035) return;
  const windTo = (cardinalDegrees(weather.windDirection) + 180) * Math.PI / 180;
  const speed = Math.max(2, weather.windMph) / 2100;
  const count = Math.round(4 + cover * (cloudSeeds.length - 4));
  const mapPhase = measuredCloudMaps.length ? seconds / 10 : 0;
  const mapA = measuredCloudMaps.length ? Math.floor(mapPhase) % measuredCloudMaps.length : 0;
  const mapB = measuredCloudMaps.length ? (mapA + 1) % measuredCloudMaps.length : 0;
  const mapBlend = mapPhase - Math.floor(mapPhase);
  ctx.save();
  ctx.beginPath(); ctx.rect(0, 0, W, HORIZON); ctx.clip();
  ctx.filter = 'blur(17px)';
  for (let i = 0; i < count; i += 1) {
    const seed = cloudSeeds[i];
    // Continuous angular advection around the sky shell; modulo wraps in one
    // direction and never reverses at an image edge.
    const shellRate = speed * (1 + seed.shell * .32);
    const u = ((seed.azimuth + seconds * Math.sin(windTo) * shellRate + 3) % 2) - 1;
    const perspective = .45 + seed.elevation * .75;
    const x = W * (.5 + u * .62 * perspective);
    const y = HORIZON * (1 - seed.elevation);
    let measured = 1;
    if (measuredCloudMaps.length) {
      const mx = clamp(Math.floor(x / W * 63), 0, 63);
      const my = clamp(Math.floor(y / HORIZON * 31), 0, 31);
      const offset = my * 64 + mx;
      measured = (measuredCloudMaps[mapA][offset] * (1 - mapBlend)
        + measuredCloudMaps[mapB][offset] * mapBlend) / 255;
      // GOES is an envelope, not literal visible luminance. Retain thin cloud
      // while suppressing invalid-shell polygon boundaries.
      measured = clamp((measured - .035) * 2.7, 0, 1);
      if (measured < .06) continue;
    }
    const width = 190 * seed.scale * perspective;
    const height = width * (.18 + .08 * seed.shell);
    const brightness = Math.round(190 + colors.day * 54);
    const alpha = (.035 + cover * .15) * (.7 + seed.shell * .12) * (.32 + .68 * measured);
    const gradient = ctx.createRadialGradient(x, y, width * .05, x, y, width);
    gradient.addColorStop(0, `rgba(${brightness},${brightness + 3},${brightness + 8},${alpha})`);
    gradient.addColorStop(1, `rgba(${brightness},${brightness + 3},${brightness + 8},0)`);
    ctx.fillStyle = gradient;
    ctx.beginPath(); ctx.ellipse(x, y, width, height, 0, 0, Math.PI * 2); ctx.fill();
  }
  ctx.restore();
}

function drawSky(seconds, sun, colors) {
  const gradient = ctx.createLinearGradient(0, 0, 0, HORIZON);
  gradient.addColorStop(0, rgba(colors.top));
  gradient.addColorStop(1, rgba(mix(colors.horizon, [243, 132, 75], colors.twilight * .62)));
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, W, HORIZON + 2);
  drawSun(sun, colors);
  drawProceduralClouds(seconds, colors);
}

function oceanMotion() {
  const toward = (sea.waveDirectionDeg + 180) * Math.PI / 180;
  const camera = CAMERA_BEARING * Math.PI / 180;
  const relative = toward - camera;
  return { right: Math.sin(relative), forward: Math.cos(relative) };
}

function drawOcean(seconds, colors) {
  const oceanHeight = H - HORIZON;
  const motion = oceanMotion();
  const waveScale = clamp(sea.waveHeightM / .55, .3, 2.7);
  const period = clamp(sea.wavePeriodS, 3, 18);
  const omega = Math.PI * 2 / period;
  const fallback = ctx.createLinearGradient(0, HORIZON, 0, H);
  fallback.addColorStop(0, rgba(mix([18, 61, 87], colors.horizon, .22)));
  fallback.addColorStop(1, rgba(mix([5, 28, 45], colors.top, .12)));
  ctx.fillStyle = fallback; ctx.fillRect(0, HORIZON, W, oceanHeight);

  const strip = 8;
  for (let y = 0; y < oceanHeight; y += strip) {
    const depth = y / oceanHeight;
    const perspective = .35 + depth * 1.8;
    const phase = y * (.025 + .02 * depth) + seconds * omega * Math.max(.2, motion.forward);
    const xShift = (Math.sin(phase) * 7 + Math.sin(phase * .47 + 1.8) * 4) * waveScale * perspective * motion.right;
    const yShift = Math.sin(phase + .9) * 2.2 * waveScale * perspective;
    ctx.globalAlpha = .92;
    ctx.drawImage(oceanTexture, 0, y, W, Math.min(strip + 2, oceanHeight - y),
      xShift, HORIZON + y + yShift, W, strip + 2);
    // Wrap exposed horizontal edge with the opposite edge of the same texture.
    if (xShift > 0) ctx.drawImage(oceanTexture, W - xShift, y, xShift, strip + 2, 0, HORIZON + y + yShift, xShift, strip + 2);
    if (xShift < 0) ctx.drawImage(oceanTexture, 0, y, -xShift, strip + 2, W + xShift, HORIZON + y + yShift, -xShift, strip + 2);
  }
  ctx.globalAlpha = 1;
  const night = 1 - colors.day;
  if (night > .02) {
    ctx.fillStyle = `rgba(0,7,17,${night * .62})`;
    ctx.fillRect(0, HORIZON, W, oceanHeight);
  }
  drawShoreFoam(seconds, waveScale, period, motion);
}

function drawShoreFoam(seconds, waveScale, period, motion) {
  const points = registration.shore_points_logical_px || [];
  if (!points.length) return;
  ctx.save();
  ctx.globalCompositeOperation = 'screen';
  ctx.lineCap = 'round';
  const sampleOffset = Math.floor(seconds * 12) % 4;
  for (let i = sampleOffset; i < points.length; i += 4) {
    const [lx, ly] = points[i];
    const x = lx * 2;
    const y = ly * 2;
    const localPhase = ((seconds / period) + hash(i * 8.31) + (x * motion.right - y * motion.forward) / 1100) % 1;
    const pulse = Math.sin(localPhase * Math.PI);
    if (pulse < .48) continue;
    const alpha = (pulse - .48) * .34 * clamp(waveScale, .55, 2.2);
    const length = 3 + hash(i * 17.2) * 11 * waveScale;
    ctx.strokeStyle = `rgba(218,239,246,${alpha})`;
    ctx.lineWidth = 1.2 + hash(i * 29.4) * 2.2;
    ctx.beginPath();
    ctx.moveTo(x - length * .5, y + 1.5);
    ctx.quadraticCurveTo(x, y - 2.5 * pulse, x + length * .5, y);
    ctx.stroke();
  }
  ctx.restore();
}

function drawForeground() {
  if (params.get('foreground') !== '0') ctx.drawImage(foreground, 0, 0, W, H);
}

function render(now) {
  const seconds = (now - startedAt) / 1000;
  const sun = sunPosition();
  const colors = palette(sun.altitudeDeg);
  drawSky(seconds, sun, colors);
  drawOcean(seconds, colors);
  drawForeground();
  requestAnimationFrame(render);
}

async function refreshWeather() {
  const data = await json(WEATHER_URL);
  const period = data.properties.periods[0];
  weather = {
    cloudCover: cloudFraction(period.shortForecast),
    windMph: Number.parseInt(period.windSpeed, 10) || 8,
    windDirection: period.windDirection || 'SW',
    temperatureF: period.temperature,
    description: period.shortForecast || 'Current conditions',
  };
}

async function refreshMarine() {
  const current = (await json(MARINE_URL)).current;
  sea = {
    waveHeightM: current.wave_height ?? sea.waveHeightM,
    waveDirectionDeg: current.wave_direction ?? sea.waveDirectionDeg,
    wavePeriodS: current.wave_period ?? sea.wavePeriodS,
    swellHeightM: current.swell_wave_height ?? sea.swellHeightM,
    swellDirectionDeg: current.swell_wave_direction ?? sea.swellDirectionDeg,
    swellPeriodS: current.swell_wave_period ?? sea.swellPeriodS,
  };
}

function updateConditions() {
  if (!conditionsEl) return;
  const temp = weather.temperatureF == null ? '' : `${weather.temperatureF}°F · `;
  const time = lastUpdated ? ` · ${lastUpdated.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}` : '';
  conditionsEl.textContent = `${temp}${weather.description} · ${weather.windDirection} ${weather.windMph} mph · ${sea.waveHeightM.toFixed(2)} m @ ${sea.wavePeriodS.toFixed(1)} s${time}`;
}

async function refreshLive() {
  const results = await Promise.allSettled([refreshWeather(), refreshMarine()]);
  const success = results.some((result) => result.status === 'fulfilled');
  if (success) {
    lastUpdated = new Date();
    localStorage.setItem(CACHE_KEY, JSON.stringify({ weather, sea, savedAt: lastUpdated.toISOString() }));
  } else {
    try {
      const cached = JSON.parse(localStorage.getItem(CACHE_KEY) || 'null');
      if (cached?.weather) weather = cached.weather;
      if (cached?.sea) sea = cached.sea;
      if (cached?.savedAt) lastUpdated = new Date(cached.savedAt);
    } catch (_) { /* deterministic defaults remain */ }
  }
  offlineEl.hidden = success;
  updateConditions();
}

async function loadMeasuredClouds() {
  try {
    const metadata = await json(`./runtime/clouds/goes-cloud-plane.json?t=${Date.now()}`);
    const count = Math.min(12, metadata.motion?.frame_count || 0);
    const generation = encodeURIComponent(metadata.source_times_utc?.at(-1) || Date.now());
    const frames = await Promise.all(Array.from({ length: count }, (_, i) =>
      image(`./runtime/clouds/cloud-frame-${String(i).padStart(3, '0')}.png?v=${generation}`)));
    measuredCloudFrames = frames;
    measuredCloudMaps = frames.map((frame) => {
      const sample = new OffscreenCanvas(64, 32);
      const sampleCtx = sample.getContext('2d', { willReadFrequently: true });
      sampleCtx.drawImage(frame, 0, 0, 1024, 291, 0, 0, 64, 32);
      const pixels = sampleCtx.getImageData(0, 0, 64, 32).data;
      const alpha = new Uint8Array(64 * 32);
      for (let i = 0; i < alpha.length; i += 1) alpha[i] = pixels[i * 4 + 3];
      return alpha;
    });
  } catch (error) {
    console.info('GOES shell frames unavailable; using wind-driven procedural fallback', error);
  }
}

async function init() {
  [registration, foreground, oceanTexture] = await Promise.all([
    json('./assets/registration.json'),
    image('./assets/cliff-house-foreground.png'),
    image('./assets/cliff-house-ocean-texture.jpg'),
  ]);
  hud.hidden = params.get('debug') !== '1' && params.get('hud') !== '1';
  await Promise.allSettled([refreshLive(), loadMeasuredClouds()]);
  requestAnimationFrame(render);
  setInterval(refreshLive, REFRESH_MS);
  setInterval(loadMeasuredClouds, REFRESH_MS);
  if ('serviceWorker' in navigator) navigator.serviceWorker.register('./sw.js').catch(() => {});
  if (params.get('fullscreen') !== '0') {
    document.addEventListener('pointerdown', () => document.documentElement.requestFullscreen?.().catch(() => {}), { once: true });
  }
}

init().catch((error) => {
  console.error(error);
  if (conditionsEl) conditionsEl.textContent = `Failed: ${error.message}`;
  hud.hidden = false;
});
