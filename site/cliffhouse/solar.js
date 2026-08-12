const LAT = 43.2206648;
const LON = -70.5795506;
const rad = (d) => d * Math.PI / 180;
const deg = (r) => r * 180 / Math.PI;
const norm360 = (v) => ((v % 360) + 360) % 360;

function refractionDeg(h) {
  if (h > 85) return 0;
  const tangent = Math.tan(rad(h));
  let arcsec;
  if (h > 5) arcsec = 58.1 / tangent - 0.07 / tangent ** 3 + 0.000086 / tangent ** 5;
  else if (h > -0.575) arcsec = 1735 + h * (-518.2 + h * (103.4 + h * (-12.79 + h * 0.711)));
  else arcsec = -20.774 / tangent;
  return arcsec / 3600;
}

export function sunPosition(now = new Date()) {
  const year = now.getUTCFullYear();
  const month = now.getUTCMonth() + 1;
  const day = now.getUTCDate();
  const hour = now.getUTCHours() + now.getUTCMinutes() / 60 + now.getUTCSeconds() / 3600;
  const a = Math.floor((14 - month) / 12);
  const y = year + 4800 - a;
  const m = month + 12 * a - 3;
  const jdn = day + Math.floor((153 * m + 2) / 5) + 365 * y
    + Math.floor(y / 4) - Math.floor(y / 100) + Math.floor(y / 400) - 32045;
  const t = (jdn + (hour - 12) / 24 - 2451545) / 36525;
  const l0 = norm360(280.46646 + t * (36000.76983 + t * 0.0003032));
  const anomaly = 357.52911 + t * (35999.05029 - t * 0.0001537);
  const eccentricity = 0.016708634 - t * (0.000042037 + t * 0.0000001267);
  const ma = rad(anomaly);
  const correction = (1.914602 - t * (0.004817 + t * 0.000014)) * Math.sin(ma)
    + (0.019993 - t * 0.000101) * Math.sin(2 * ma) + 0.000289 * Math.sin(3 * ma);
  const omega = 125.04 - 1934.136 * t;
  const lambda = l0 + correction - 0.00569 - 0.00478 * Math.sin(rad(omega));
  const epsilon0 = 23 + (26 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60) / 60;
  const epsilon = epsilon0 + 0.00256 * Math.cos(rad(omega));
  const declination = Math.asin(Math.sin(rad(epsilon)) * Math.sin(rad(lambda)));
  const yTerm = Math.tan(rad(epsilon / 2)) ** 2;
  const l0r = rad(l0);
  const eqTime = 4 * deg(yTerm * Math.sin(2 * l0r) - 2 * eccentricity * Math.sin(ma)
    + 4 * eccentricity * yTerm * Math.sin(ma) * Math.cos(2 * l0r)
    - 0.5 * yTerm ** 2 * Math.sin(4 * l0r) - 1.25 * eccentricity ** 2 * Math.sin(2 * ma));
  const solarMinutes = ((hour * 60 + eqTime + 4 * LON) % 1440 + 1440) % 1440;
  let hourAngle = solarMinutes / 4 - 180;
  if (hourAngle < -180) hourAngle += 360;
  const ha = rad(hourAngle);
  const latitude = rad(LAT);
  const zenith = Math.acos(Math.sin(latitude) * Math.sin(declination)
    + Math.cos(latitude) * Math.cos(declination) * Math.cos(ha));
  const trueAltitude = 90 - deg(zenith);
  const altitudeDeg = trueAltitude + refractionDeg(trueAltitude);
  let denominator = Math.cos(latitude) * Math.sin(zenith);
  if (Math.abs(denominator) < 1e-9) denominator = 1e-9;
  let azimuthDeg = deg(Math.acos(Math.max(-1, Math.min(1,
    (Math.sin(latitude) * Math.cos(zenith) - Math.sin(declination)) / denominator))));
  azimuthDeg = hourAngle > 0 ? norm360(azimuthDeg + 180) : norm360(540 - azimuthDeg);
  return { altitudeDeg, azimuthDeg };
}

export function signedAngle(degrees) {
  let value = norm360(degrees);
  if (value > 180) value -= 360;
  return value;
}
