(() => {
  'use strict';

  const WIDTH = 1024;
  const HEIGHT = 600;
  const HORIZON = 291;
  const YORK = { latitude: 43.1637, longitude: -70.648 };
  const WEATHER_URL = 'https://api.weather.gov/gridpoints/GYX/64,33/forecast/hourly';
  const MARINE_URL = 'https://marine-api.open-meteo.com/v1/marine?latitude=43.165&longitude=-70.59&current=wave_height,wave_direction,wave_period,swell_wave_height,swell_wave_direction,swell_wave_period,wind_wave_height,wind_wave_direction,wind_wave_period&timezone=America%2FNew_York';
  const LIVE_CACHE = 'nubble-live-v3';
  const REFRESH_MS = 10 * 60 * 1000;
  const params = new URLSearchParams(location.search);

  const canvas = document.querySelector('#scene');
  const ctx = canvas.getContext('2d', { alpha: false });
  const conditionsEl = document.querySelector('#conditions');
  const offlineEl = document.querySelector('#offline');
  const hud = document.querySelector('.hud');
  const installButton = document.querySelector('#install');
  const fullscreenButton = document.querySelector('#fullscreen');
  const hideButton = document.querySelector('#hud-toggle');
  const showButton = document.querySelector('#show-hud');

  const base = loadImage('/nubble/nubble-ocean.jpg');
  const island = loadImage('/nubble/nubble-island.png?v=6');
  const hudRequested = params.get('hud') === '1' || params.get('debug') === '1';
  let islandVisible = params.get('island') !== '0';
  let deferredInstall = null;
  let startedAt = performance.now();
  let lastUpdated = null;
  let weather = {
    cloudCover: 20,
    windMph: 8,
    windDirection: 'SW',
    temperatureF: null,
    description: 'Live sea and sky',
  };
  let sea = {
    waveHeightM: 0.4,
    waveDirectionDeg: 145,
    wavePeriodS: 7,
    swellHeightM: 0.25,
    swellDirectionDeg: 150,
    swellPeriodS: 9,
  };

  const cloudSeeds = Array.from({ length: 38 }, (_, index) => ({
    x: hash(index * 13.1) * WIDTH,
    y: 28 + hash(index * 41.7) * (HORIZON - 82),
    scale: 0.45 + hash(index * 91.3) * 1.25,
    depth: 0.35 + hash(index * 7.9) * 0.65,
  }));

  const foamSeeds = Array.from({ length: 18 }, (_, index) => ({
    depth: 0.08 + hash(index * 5.2) * 0.9,
    phase: hash(index * 17.4),
    length: 55 + hash(index * 29.2) * 190,
    x: hash(index * 67.9) * WIDTH,
  }));

  function loadImage(source) {
    const image = new Image();
    image.decoding = 'async';
    image.src = source;
    return image;
  }

  function hash(value) {
    return Math.abs(Math.sin(value * 12.9898) * 43758.5453) % 1;
  }

  function clamp(value, min, max) {
    return Math.min(max, Math.max(min, value));
  }

  function cardinalToDegrees(direction) {
    const names = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
    const index = names.indexOf(direction);
    return index < 0 ? 225 : index * 22.5;
  }

  function solarAltitude(date = new Date()) {
    const radians = Math.PI / 180;
    const dayStart = Date.UTC(date.getUTCFullYear(), 0, 0);
    const day = Math.floor((date.getTime() - dayStart) / 86400000);
    const hour = date.getUTCHours() + date.getUTCMinutes() / 60 + date.getUTCSeconds() / 3600;
    const gamma = 2 * Math.PI / 365 * (day - 1 + (hour - 12) / 24);
    const declination = 0.006918 - 0.399912 * Math.cos(gamma) + 0.070257 * Math.sin(gamma)
      - 0.006758 * Math.cos(2 * gamma) + 0.000907 * Math.sin(2 * gamma)
      - 0.002697 * Math.cos(3 * gamma) + 0.00148 * Math.sin(3 * gamma);
    const equation = 229.18 * (0.000075 + 0.001868 * Math.cos(gamma) - 0.032077 * Math.sin(gamma)
      - 0.014615 * Math.cos(2 * gamma) - 0.040849 * Math.sin(2 * gamma));
    const minutes = date.getUTCHours() * 60 + date.getUTCMinutes() + date.getUTCSeconds() / 60;
    const solarMinutes = (minutes + equation + 4 * YORK.longitude + 1440) % 1440;
    const hourAngle = (solarMinutes / 4 - 180) * radians;
    const latitude = YORK.latitude * radians;
    const zenith = Math.acos(
      Math.sin(latitude) * Math.sin(declination)
      + Math.cos(latitude) * Math.cos(declination) * Math.cos(hourAngle),
    );
    return 90 - zenith / radians;
  }

  function paletteForSun(altitude) {
    const daylight = clamp((altitude + 8) / 24, 0, 1);
    const twilight = clamp(1 - Math.abs(altitude + 2) / 12, 0, 1);
    return {
      daylight,
      skyTop: mix([3, 10, 22], [76, 151, 205], daylight),
      skyHorizon: mix([11, 18, 31], [180, 214, 235], daylight),
      warm: twilight,
    };
  }

  function mix(a, b, amount) {
    return a.map((value, index) => Math.round(value + (b[index] - value) * amount));
  }

  function rgb(values, alpha = 1) {
    return `rgba(${values[0]}, ${values[1]}, ${values[2]}, ${alpha})`;
  }

  function drawSky(seconds, palette) {
    const gradient = ctx.createLinearGradient(0, 0, 0, HORIZON);
    gradient.addColorStop(0, rgb(palette.skyTop));
    gradient.addColorStop(1, rgb(mix(palette.skyHorizon, [232, 138, 88], palette.warm * 0.52)));
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, WIDTH, HORIZON);

    if (palette.daylight < 0.18) drawStars(seconds, 1 - palette.daylight / 0.18);
    drawClouds(seconds, palette);
  }

  function drawStars(seconds, opacity) {
    ctx.save();
    ctx.fillStyle = `rgba(236, 244, 255, ${0.72 * opacity})`;
    for (let index = 0; index < 88; index += 1) {
      const x = (hash(index * 73.1) * WIDTH + seconds * (0.02 + hash(index) * 0.01)) % WIDTH;
      const y = 10 + hash(index * 31.4) * (HORIZON - 38);
      const radius = 0.35 + hash(index * 11.2) * 0.85;
      ctx.beginPath();
      ctx.arc(x, y, radius, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.restore();
  }

  function drawClouds(seconds, palette) {
    const cover = clamp(weather.cloudCover / 100, 0, 1);
    if (cover < 0.04) return;

    const direction = cardinalToDegrees(weather.windDirection) * Math.PI / 180;
    const velocity = Math.max(1.5, weather.windMph) * 0.7;
    const xVelocity = -Math.sin(direction) * velocity;
    const visibleCount = Math.round(4 + cover * (cloudSeeds.length - 4));

    ctx.save();
    ctx.filter = 'blur(8px)';
    for (let index = 0; index < visibleCount; index += 1) {
      const seed = cloudSeeds[index];
      const span = WIDTH + 300;
      const x = ((seed.x + seconds * xVelocity * seed.depth + span) % span) - 150;
      const width = (105 + cover * 95) * seed.scale;
      const height = width * (0.19 + seed.depth * 0.08);
      const shade = Math.round(205 + palette.daylight * 38);
      const alpha = (0.06 + cover * 0.18) * (0.55 + seed.depth * 0.45);
      const cloudGradient = ctx.createRadialGradient(x, seed.y, 4, x, seed.y, width * 0.55);
      cloudGradient.addColorStop(0, `rgba(${shade}, ${shade + 3}, ${shade + 7}, ${alpha})`);
      cloudGradient.addColorStop(1, `rgba(${shade}, ${shade + 3}, ${shade + 7}, 0)`);
      ctx.fillStyle = cloudGradient;
      ctx.beginPath();
      ctx.ellipse(x, seed.y, width, height, 0, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.restore();
  }

  function drawOcean(seconds, palette) {
    if (!base.complete || base.naturalWidth === 0) {
      ctx.fillStyle = '#0b2940';
      ctx.fillRect(0, HORIZON, WIDTH, HEIGHT - HORIZON);
      return;
    }

    const heightScale = clamp(sea.waveHeightM / 0.55, 0.35, 2.2);
    const period = clamp(sea.wavePeriodS, 3, 16);
    const direction = sea.waveDirectionDeg * Math.PI / 180;
    const directionX = Math.sin(direction);
    const phase = seconds * Math.PI * 2 / period;

    for (let y = HORIZON; y < HEIGHT; y += 6) {
      const depth = (y - HORIZON) / (HEIGHT - HORIZON);
      const shift = Math.sin(phase + y * 0.043) * (0.8 + depth * 5.5) * heightScale
        + Math.sin(phase * 0.53 + y * 0.019) * depth * 3 * directionX;
      ctx.drawImage(base, 0, y, WIDTH, 6, shift, y, WIDTH, 6);
      if (shift > 0) ctx.drawImage(base, WIDTH - shift, y, shift, 6, 0, y, shift, 6);
      if (shift < 0) ctx.drawImage(base, 0, y, -shift, 6, WIDTH + shift, y, -shift, 6);
    }

    ctx.save();
    ctx.globalCompositeOperation = 'screen';
    ctx.setLineDash([10, 7, 4, 12]);
    for (const foam of foamSeeds) {
      const cycle = (seconds / period + foam.phase) % 1;
      const depth = clamp((foam.depth * 0.48 + cycle * 0.62), 0.04, 1);
      const y = HORIZON + Math.pow(depth, 1.65) * (HEIGHT - HORIZON);
      const x = foam.x + directionX * cycle * 22;
      const amplitude = (0.8 + depth * 3.2) * heightScale;
      ctx.strokeStyle = `rgba(220, 239, 248, ${0.035 + depth * 0.13 * heightScale})`;
      ctx.lineWidth = 0.5 + depth * 0.9;
      ctx.beginPath();
      for (let px = 0; px < foam.length; px += 5) {
        const py = y + Math.sin(px * 0.07 + phase + foam.phase * Math.PI * 2) * amplitude;
        if (px === 0) ctx.moveTo(x + px, py);
        else ctx.lineTo(x + px, py);
      }
      ctx.stroke();
    }
    ctx.restore();

    const night = 1 - palette.daylight;
    if (night > 0.02) {
      ctx.fillStyle = `rgba(0, 8, 19, ${night * 0.58})`;
      ctx.fillRect(0, HORIZON, WIDTH, HEIGHT - HORIZON);
    }
  }

  function drawIsland() {
    if (islandVisible && island.complete && island.naturalWidth > 0) {
      ctx.drawImage(island, 0, 0, WIDTH, HEIGHT);
    }
  }

  function render(now) {
    const seconds = (now - startedAt) / 1000;
    const palette = paletteForSun(solarAltitude());
    drawSky(seconds, palette);
    drawOcean(seconds, palette);
    drawIsland();
    requestAnimationFrame(render);
  }

  async function fetchJson(url) {
    const response = await fetch(url, {
      headers: { Accept: 'application/json, application/geo+json' },
      cache: 'no-store',
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return response.json();
  }

  async function refreshWeather() {
    const data = await fetchJson(WEATHER_URL);
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
    const data = await fetchJson(MARINE_URL);
    const current = data.current;
    sea = {
      waveHeightM: current.wave_height ?? sea.waveHeightM,
      waveDirectionDeg: current.wave_direction ?? sea.waveDirectionDeg,
      wavePeriodS: current.wave_period ?? sea.wavePeriodS,
      swellHeightM: current.swell_wave_height ?? sea.swellHeightM,
      swellDirectionDeg: current.swell_wave_direction ?? sea.swellDirectionDeg,
      swellPeriodS: current.swell_wave_period ?? sea.swellPeriodS,
    };
  }

  async function refreshLiveData() {
    const results = await Promise.allSettled([refreshWeather(), refreshMarine()]);
    const succeeded = results.some((result) => result.status === 'fulfilled');
    if (succeeded) {
      lastUpdated = new Date();
      localStorage.setItem(LIVE_CACHE, JSON.stringify({ weather, sea, savedAt: lastUpdated.toISOString() }));
    } else {
      try {
        const cached = JSON.parse(localStorage.getItem(LIVE_CACHE) || 'null');
        if (cached?.weather) weather = cached.weather;
        if (cached?.sea) sea = cached.sea;
        if (cached?.savedAt) lastUpdated = new Date(cached.savedAt);
      } catch (_) {
        // Keep safe defaults if a previous cache is corrupt.
      }
    }
    offlineEl.hidden = succeeded;
    updateConditions();
  }

  function cloudFraction(description = '') {
    const text = description.toLowerCase();
    if (text.includes('overcast')) return 96;
    if (text.includes('mostly cloudy')) return 78;
    if (text.includes('partly cloudy')) return 48;
    if (text.includes('mostly clear')) return 18;
    if (text.includes('clear') || text.includes('sunny')) return 7;
    if (text.includes('fog') || text.includes('rain') || text.includes('snow')) return 88;
    if (text.includes('cloud')) return 62;
    return 30;
  }

  function updateConditions() {
    const temperature = weather.temperatureF == null ? '' : `${weather.temperatureF}°F · `;
    const wave = `${sea.waveHeightM.toFixed(2)} m @ ${sea.wavePeriodS.toFixed(1)} s`;
    const updated = lastUpdated ? ` · updated ${lastUpdated.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}` : '';
    conditionsEl.textContent = `${temperature}${weather.description} · wind ${weather.windDirection} ${weather.windMph} mph · waves ${wave}${updated}`;
  }

  function setHudVisible(visible) {
    hud.classList.toggle('is-hidden', !visible);
    showButton.hidden = !hudRequested || visible;
  }

  function setIslandVisible(visible, updateUrl = true) {
    islandVisible = visible;
    if (!updateUrl) return;
    const next = new URL(location.href);
    if (visible) next.searchParams.delete('island');
    else next.searchParams.set('island', '0');
    history.replaceState(null, '', next);
  }

  async function toggleFullscreen() {
    if (!document.fullscreenElement) await document.documentElement.requestFullscreen?.();
    else await document.exitFullscreen?.();
  }

  window.addEventListener('beforeinstallprompt', (event) => {
    event.preventDefault();
    deferredInstall = event;
    installButton.hidden = false;
  });

  installButton.addEventListener('click', async () => {
    if (!deferredInstall) return;
    deferredInstall.prompt();
    await deferredInstall.userChoice;
    deferredInstall = null;
    installButton.hidden = true;
  });

  fullscreenButton.addEventListener('click', toggleFullscreen);
  hideButton.addEventListener('click', () => setHudVisible(false));
  showButton.addEventListener('click', () => setHudVisible(true));

  window.addEventListener('keydown', (event) => {
    if (event.key.toLowerCase() === 'i') setIslandVisible(!islandVisible);
    if (event.key.toLowerCase() === 'h') setHudVisible(hud.classList.contains('is-hidden'));
    if (event.key.toLowerCase() === 'f') toggleFullscreen();
  });
  window.addEventListener('online', refreshLiveData);
  window.addEventListener('offline', () => { offlineEl.hidden = false; });

  if ('serviceWorker' in navigator) {
    let reloadingForUpdate = false;
    navigator.serviceWorker.addEventListener('controllerchange', () => {
      if (reloadingForUpdate) return;
      reloadingForUpdate = true;
      location.reload();
    });
    window.addEventListener('load', async () => {
      const registration = await navigator.serviceWorker.register('/nubble/sw.js?v=6', { updateViaCache: 'none' });
      registration.update();
    });
  }

  if (params.get('fullscreen') !== '0' && !matchMedia('(display-mode: fullscreen)').matches) {
    document.addEventListener('pointerdown', () => {
      if (!document.fullscreenElement) document.documentElement.requestFullscreen?.().catch(() => {});
    }, { once: true });
  }

  base.addEventListener('load', () => { startedAt = performance.now(); });
  setIslandVisible(islandVisible, false);
  setHudVisible(hudRequested);
  updateConditions();
  refreshLiveData();
  setInterval(refreshLiveData, REFRESH_MS);
  requestAnimationFrame(render);
})();
