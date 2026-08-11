(() => {
  'use strict';

  const WIDTH = 1024;
  const HEIGHT = 600;
  const HORIZON = 291;
  const YORK = { latitude: 43.1637, longitude: -70.648 };
  const WEATHER_URL = 'https://api.weather.gov/gridpoints/GYX/64,33/forecast/hourly';
  const WEATHER_CACHE = 'nubble-weather-v1';

  const canvas = document.querySelector('#scene');
  const ctx = canvas.getContext('2d', { alpha: false });
  const conditionsEl = document.querySelector('#conditions');
  const offlineEl = document.querySelector('#offline');
  const hud = document.querySelector('.hud');
  const installButton = document.querySelector('#install');
  const fullscreenButton = document.querySelector('#fullscreen');
  const hideButton = document.querySelector('#hud-toggle');
  const showButton = document.querySelector('#show-hud');

  const base = new Image();
  base.decoding = 'async';
  base.src = '/nubble/nubble-ocean.jpg';

  let deferredInstall = null;
  let startedAt = performance.now();
  let weather = {
    cloudCover: 20,
    windMph: 8,
    windDirection: 'SW',
    temperatureF: null,
    description: 'Live sea and sky',
  };

  const cloudSeeds = Array.from({ length: 38 }, (_, index) => ({
    x: hash(index * 13.1) * WIDTH,
    y: 28 + hash(index * 41.7) * (HORIZON - 82),
    scale: 0.45 + hash(index * 91.3) * 1.25,
    depth: 0.35 + hash(index * 7.9) * 0.65,
  }));

  const foamSeeds = Array.from({ length: 14 }, (_, index) => ({
    y: HORIZON + 42 + index * 19 + hash(index * 5.2) * 12,
    phase: hash(index * 17.4) * Math.PI * 2,
    length: 90 + hash(index * 29.2) * 250,
    x: hash(index * 67.9) * WIDTH,
  }));

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

  function drawSky(time, palette) {
    const gradient = ctx.createLinearGradient(0, 0, 0, HORIZON);
    gradient.addColorStop(0, rgb(palette.skyTop));
    const horizonColor = mix(palette.skyHorizon, [232, 138, 88], palette.warm * 0.52);
    gradient.addColorStop(1, rgb(horizonColor));
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, WIDTH, HORIZON);

    if (palette.daylight < 0.18) {
      drawStars(time, 1 - palette.daylight / 0.18);
    }

    drawClouds(time, palette);
  }

  function drawStars(time, opacity) {
    ctx.save();
    ctx.fillStyle = `rgba(236, 244, 255, ${0.72 * opacity})`;
    for (let index = 0; index < 88; index += 1) {
      const x = (hash(index * 73.1) * WIDTH + time * (0.002 + hash(index) * 0.001)) % WIDTH;
      const y = 10 + hash(index * 31.4) * (HORIZON - 38);
      const radius = 0.35 + hash(index * 11.2) * 0.85;
      ctx.beginPath();
      ctx.arc(x, y, radius, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.restore();
  }

  function drawClouds(time, palette) {
    const cover = clamp(weather.cloudCover / 100, 0, 1);
    if (cover < 0.04) return;

    const wind = Math.max(1.5, weather.windMph) * 0.012;
    const direction = cardinalToDegrees(weather.windDirection) * Math.PI / 180;
    const xVelocity = -Math.sin(direction) * wind;
    const visibleCount = Math.round(5 + cover * (cloudSeeds.length - 5));

    ctx.save();
    ctx.filter = 'blur(8px)';
    for (let index = 0; index < visibleCount; index += 1) {
      const seed = cloudSeeds[index];
      const drift = time * xVelocity * seed.depth;
      const x = ((seed.x + drift) % (WIDTH + 260)) - 130;
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

  function drawOcean(time, palette) {
    if (!base.complete || base.naturalWidth === 0) {
      ctx.fillStyle = '#0b2940';
      ctx.fillRect(0, HORIZON, WIDTH, HEIGHT - HORIZON);
      return;
    }

    const windScale = clamp(weather.windMph / 18, 0.25, 1.7);
    for (let y = HORIZON; y < HEIGHT; y += 2) {
      const depth = (y - HORIZON) / (HEIGHT - HORIZON);
      const shift = Math.sin(time * 0.0011 + y * 0.082) * (1 + depth * 7) * windScale
        + Math.sin(time * 0.00047 + y * 0.031) * depth * 5;
      ctx.drawImage(base, 0, y, WIDTH, 2, shift, y, WIDTH, 2);
      if (shift > 0) ctx.drawImage(base, WIDTH - shift, y, shift, 2, 0, y, shift, 2);
      if (shift < 0) ctx.drawImage(base, 0, y, -shift, 2, WIDTH + shift, y, -shift, 2);
    }

    ctx.save();
    ctx.globalCompositeOperation = 'screen';
    for (const foam of foamSeeds) {
      const depth = (foam.y - HORIZON) / (HEIGHT - HORIZON);
      const travel = (time * (0.008 + depth * 0.016)) % (WIDTH + foam.length);
      const x = ((foam.x - travel + WIDTH + foam.length) % (WIDTH + foam.length)) - foam.length;
      const amplitude = 1.5 + depth * 5;
      ctx.strokeStyle = `rgba(214, 235, 244, ${0.08 + depth * 0.18})`;
      ctx.lineWidth = 0.7 + depth * 1.2;
      ctx.beginPath();
      for (let px = 0; px < foam.length; px += 5) {
        const py = foam.y + Math.sin(px * 0.075 + time * 0.002 + foam.phase) * amplitude;
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

  function render(now) {
    const time = now - startedAt;
    const palette = paletteForSun(solarAltitude());
    drawSky(time, palette);
    drawOcean(time, palette);
    requestAnimationFrame(render);
  }

  async function refreshWeather() {
    try {
      const response = await fetch(WEATHER_URL, {
        headers: { Accept: 'application/geo+json' },
        cache: 'no-store',
      });
      if (!response.ok) throw new Error(`Weather HTTP ${response.status}`);
      const data = await response.json();
      const period = data.properties.periods[0];
      weather = {
        cloudCover: cloudFraction(period.shortForecast),
        windMph: Number.parseInt(period.windSpeed, 10) || 8,
        windDirection: period.windDirection || 'SW',
        temperatureF: period.temperature,
        description: period.shortForecast || 'Current conditions',
      };
      localStorage.setItem(WEATHER_CACHE, JSON.stringify({ weather, savedAt: Date.now() }));
      offlineEl.hidden = true;
      updateConditions();
    } catch (error) {
      const cached = JSON.parse(localStorage.getItem(WEATHER_CACHE) || 'null');
      if (cached?.weather) weather = cached.weather;
      offlineEl.hidden = navigator.onLine;
      updateConditions();
    }
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
    conditionsEl.textContent = `${temperature}${weather.description} · wind ${weather.windDirection} ${weather.windMph} mph`;
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

  fullscreenButton.addEventListener('click', async () => {
    if (!document.fullscreenElement) await document.documentElement.requestFullscreen?.();
    else await document.exitFullscreen?.();
  });

  hideButton.addEventListener('click', () => {
    hud.classList.add('is-hidden');
    showButton.hidden = false;
  });

  showButton.addEventListener('click', () => {
    hud.classList.remove('is-hidden');
    showButton.hidden = true;
  });

  window.addEventListener('online', refreshWeather);
  window.addEventListener('offline', () => { offlineEl.hidden = false; });

  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => navigator.serviceWorker.register('/nubble/sw.js'));
  }

  base.addEventListener('load', () => { startedAt = performance.now(); });
  updateConditions();
  refreshWeather();
  setInterval(refreshWeather, 15 * 60 * 1000);
  requestAnimationFrame(render);
})();
