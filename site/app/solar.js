// Solar altitude/azimuth for Cape Neddick (Nubble) from the device clock, so
// the sky tracks the real day continuously and offline. Direct port of the
// Android app's SolarPosition.kt (NOAA low-precision algorithm + refraction).
const LAT = 43.16530;
const LON = -70.59110;
const rad = (d) => (d * Math.PI) / 180;
const deg = (r) => (r * 180) / Math.PI;
function norm360(v) { let r = v % 360; if (r < 0) r += 360; return r; }

function refractionDeg(h) {
  if (h > 85) return 0;
  const te = Math.tan(rad(h));
  let arcsec;
  if (h > 5) arcsec = 58.1 / te - 0.07 / te ** 3 + 0.000086 / te ** 5;
  else if (h > -0.575) arcsec = 1735 + h * (-518.2 + h * (103.4 + h * (-12.79 + h * 0.711)));
  else arcsec = -20.774 / te;
  return arcsec / 3600;
}

// Returns { altitudeDeg, azimuthDeg }.
export function sunPosition(now = new Date()) {
  const year = now.getUTCFullYear();
  const month = now.getUTCMonth() + 1;
  const day = now.getUTCDate();
  const hour = now.getUTCHours() + now.getUTCMinutes() / 60 + now.getUTCSeconds() / 3600;

  const a = Math.floor((14 - month) / 12);
  const y = year + 4800 - a;
  const m = month + 12 * a - 3;
  const jdn = day + Math.floor((153 * m + 2) / 5) + 365 * y +
    Math.floor(y / 4) - Math.floor(y / 100) + Math.floor(y / 400) - 32045;
  const jd = jdn + (hour - 12) / 24;
  const t = (jd - 2451545) / 36525;

  const l0 = norm360(280.46646 + t * (36000.76983 + t * 0.0003032));
  const mAnom = 357.52911 + t * (35999.05029 - t * 0.0001537);
  const e = 0.016708634 - t * (0.000042037 + t * 0.0000001267);
  const mRad = rad(mAnom);
  const c = (1.914602 - t * (0.004817 + t * 0.000014)) * Math.sin(mRad) +
    (0.019993 - t * 0.000101) * Math.sin(2 * mRad) + 0.000289 * Math.sin(3 * mRad);
  const trueLong = l0 + c;
  const omega = 125.04 - 1934.136 * t;
  const lambda = trueLong - 0.00569 - 0.00478 * Math.sin(rad(omega));
  const epsilon0 = 23 + (26 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60) / 60;
  const epsilon = epsilon0 + 0.00256 * Math.cos(rad(omega));
  const declination = Math.asin(Math.sin(rad(epsilon)) * Math.sin(rad(lambda)));

  const yTerm = Math.tan(rad(epsilon / 2)) ** 2;
  const l0Rad = rad(l0);
  const eqTime = 4 * deg(
    yTerm * Math.sin(2 * l0Rad) - 2 * e * Math.sin(mRad) +
    4 * e * yTerm * Math.sin(mRad) * Math.cos(2 * l0Rad) -
    0.5 * yTerm * yTerm * Math.sin(4 * l0Rad) - 1.25 * e * e * Math.sin(2 * mRad),
  );
  const trueSolarTime = ((hour * 60 + eqTime + 4 * LON) % 1440 + 1440) % 1440;
  let hourAngle = trueSolarTime / 4 - 180;
  if (hourAngle < -180) hourAngle += 360;
  const haRad = rad(hourAngle);

  const latRad = rad(LAT);
  const zenith = Math.acos(
    Math.sin(latRad) * Math.sin(declination) +
    Math.cos(latRad) * Math.cos(declination) * Math.cos(haRad),
  );
  const trueAltitude = 90 - deg(zenith);
  const altitude = trueAltitude + refractionDeg(trueAltitude);

  let denom = Math.cos(latRad) * Math.sin(zenith);
  denom = Math.max(-1, Math.min(1, denom));
  if (Math.abs(denom) < 1e-9) denom = 1e-9;
  let azimuth = deg(Math.acos(
    Math.max(-1, Math.min(1, (Math.sin(latRad) * Math.cos(zenith) - Math.sin(declination)) / denom)),
  ));
  azimuth = hourAngle > 0 ? norm360(azimuth + 180) : norm360(540 - azimuth);
  return { altitudeDeg: altitude, azimuthDeg: azimuth };
}
