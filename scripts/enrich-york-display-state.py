#!/usr/bin/env python3
"""Add York-local sun/moon state to a fetched Luminary condition document.

The calculation is intentionally dependency-free so the exact same compact
math can be ported to the P4.  It is appropriate for display-art direction
(day/twilight/night, moon phase and whether a moon belongs in the frame), not
for navigational astronomy.
"""

from __future__ import annotations

import argparse
import json
import math
from datetime import datetime, timezone
from pathlib import Path


def clamp_degrees(value: float) -> float:
    return value % 360.0


def julian_day(instant: datetime) -> float:
    instant = instant.astimezone(timezone.utc)
    year, month = instant.year, instant.month
    if month <= 2:
        year -= 1
        month += 12
    day = instant.day + (instant.hour + (instant.minute + (instant.second + instant.microsecond / 1e6) / 60) / 60) / 24
    a = year // 100
    b = 2 - a + a // 4
    return math.floor(365.25 * (year + 4716)) + math.floor(30.6001 * (month + 1)) + day + b - 1524.5


def horizontal_position(ra_deg: float, dec_deg: float, jd: float,
                        latitude_deg: float, longitude_deg: float) -> tuple[float, float]:
    gmst = clamp_degrees(280.46061837 + 360.98564736629 * (jd - 2451545.0))
    hour_angle = math.radians(clamp_degrees(gmst + longitude_deg - ra_deg) + 180.0) - math.pi
    latitude = math.radians(latitude_deg)
    declination = math.radians(dec_deg)
    altitude_deg = math.degrees(math.asin(math.sin(latitude) * math.sin(declination) +
                                         math.cos(latitude) * math.cos(declination) * math.cos(hour_angle)))
    azimuth_deg = clamp_degrees(math.degrees(math.atan2(
        math.sin(hour_angle),
        math.cos(hour_angle) * math.sin(latitude) - math.tan(declination) * math.cos(latitude))) + 180.0)
    return altitude_deg, azimuth_deg


def solar_position(jd: float, latitude_deg: float, longitude_deg: float) -> tuple[float, float, float]:
    d = jd - 2451545.0
    mean_longitude = clamp_degrees(280.460 + 0.9856474 * d)
    anomaly = math.radians(clamp_degrees(357.528 + 0.9856003 * d))
    ecliptic_longitude = math.radians(clamp_degrees(mean_longitude + 1.915 * math.sin(anomaly) + 0.020 * math.sin(2 * anomaly)))
    obliquity = math.radians(23.439 - 0.0000004 * d)
    ra = math.degrees(math.atan2(math.cos(obliquity) * math.sin(ecliptic_longitude), math.cos(ecliptic_longitude)))
    dec = math.degrees(math.asin(math.sin(obliquity) * math.sin(ecliptic_longitude)))
    altitude_deg, azimuth_deg = horizontal_position(ra, dec, jd, latitude_deg, longitude_deg)
    return altitude_deg, azimuth_deg, clamp_degrees(math.degrees(ecliptic_longitude))


def lunar_position(jd: float, latitude_deg: float, longitude_deg: float,
                   sun_lon_deg: float) -> tuple[float, float, float]:
    # Low-precision geocentric lunar orbit (sufficient to decide moon art).
    d = jd - 2451543.5
    ascending = math.radians(clamp_degrees(125.1228 - 0.0529538083 * d))
    inclination = math.radians(5.1454)
    periapsis = math.radians(clamp_degrees(318.0634 + 0.1643573223 * d))
    eccentricity = 0.054900
    mean_anomaly = math.radians(clamp_degrees(115.3654 + 13.0649929509 * d))
    eccentric_anomaly = mean_anomaly + eccentricity * math.sin(mean_anomaly) * (1 + eccentricity * math.cos(mean_anomaly))
    x = math.cos(eccentric_anomaly) - eccentricity
    y = math.sqrt(1 - eccentricity * eccentricity) * math.sin(eccentric_anomaly)
    true_anomaly = math.atan2(y, x)
    ecliptic_lon = math.atan2(math.sin(true_anomaly + periapsis) * math.cos(inclination),
                              math.cos(true_anomaly + periapsis)) + ascending
    ecliptic_lat = math.asin(math.sin(true_anomaly + periapsis) * math.sin(inclination))
    obliquity = math.radians(23.4393)
    ra = math.degrees(math.atan2(math.sin(ecliptic_lon) * math.cos(obliquity) -
                                  math.tan(ecliptic_lat) * math.sin(obliquity), math.cos(ecliptic_lon)))
    dec = math.degrees(math.asin(math.sin(ecliptic_lat) * math.cos(obliquity) +
                                 math.cos(ecliptic_lat) * math.sin(obliquity) * math.sin(ecliptic_lon)))
    phase = (clamp_degrees(math.degrees(ecliptic_lon) - sun_lon_deg) / 360.0) % 1.0
    illumination = (1.0 - math.cos(phase * 2.0 * math.pi)) / 2.0
    altitude_deg, azimuth_deg = horizontal_position(ra, dec, jd, latitude_deg, longitude_deg)
    return altitude_deg, azimuth_deg, illumination


def sun_state(altitude_deg: float) -> str:
    if altitude_deg >= 0:
        return "day"
    if altitude_deg >= -6:
        return "civil_twilight"
    if altitude_deg >= -12:
        return "nautical_twilight"
    return "night"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="JSON from fetch-york-conditions.py")
    parser.add_argument("--output", type=Path, help="Defaults to stdout")
    parser.add_argument("--visual-observation", type=Path,
                        help="Live-camera palette/coverage calibration; overrides visible sky state, not geometry")
    args = parser.parse_args()

    state = json.loads(args.input.read_text())
    if args.visual_observation:
        observation = json.loads(args.visual_observation.read_text())
        if observation.get("confidence", 0.0) >= 0.45:
            state["visual_observation"] = observation
            state["meteorological_sky"] = state.get("sky")
            state["sky"] = observation["sky_state"]
    instant_text = state.get("updated_at") or state.get("observed_at")
    if not instant_text:
        raise ValueError("condition document has no updated_at or observed_at")
    instant = datetime.fromisoformat(instant_text.replace("Z", "+00:00"))
    latitude, longitude = state["location"]["latitude"], state["location"]["longitude"]
    jd = julian_day(instant)
    sun_altitude, sun_azimuth, sun_lon = solar_position(jd, latitude, longitude)
    moon_altitude, moon_azimuth, illumination = lunar_position(jd, latitude, longitude, sun_lon)
    state["sun"] = {"state": sun_state(sun_altitude), "altitude_deg": round(sun_altitude, 1),
                    "azimuth_deg": round(sun_azimuth, 1), "off_screen": True}
    state["moon"] = {"altitude_deg": round(moon_altitude, 1), "azimuth_deg": round(moon_azimuth, 1),
                     "illumination": round(illumination, 3),
                     "visible": moon_altitude > 3 and sun_altitude < -3}
    state["render_profile"] = {
        "cloud_motion": "slow" if state.get("sky") in ("clear", "few") else "layered",
        "sea_energy": ("energetic" if (state.get("wave_height_m") or 0) >= 1.5 else
                        "moderate" if (state.get("wave_height_m") or 0) >= 0.6 else "calm"),
        "tide_phase": state.get("tide", {}).get("phase"),
        "horizon_y_px": 291,
        "camera": "locked",
        "sky_palette_source": ("live_nubble_camera" if "visual_observation" in state else
                               "meteorological_authored_fallback"),
    }
    encoded = json.dumps(state, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
