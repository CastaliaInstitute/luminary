#!/usr/bin/env python3
"""Produce the compact live-condition document consumed by a Luminary viewer.

This is deliberately a host-side reference collector: it makes the NOAA/NWS
contracts observable and testable before the same small fetch/mapping logic is
embedded in the ESP32 viewer.  Missing services degrade to an explicit field;
they never result in made-up weather.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from urllib.request import Request, urlopen
from zoneinfo import ZoneInfo


USER_AGENT = "Luminary/0.1 (contact@castalia.institute)"


def get_json(url: str) -> dict:
    request = Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/geo+json, application/json"})
    with urlopen(request, timeout=15) as response:
        return json.load(response)


def get_text(url: str) -> str:
    request = Request(url, headers={"User-Agent": USER_AGENT})
    with urlopen(request, timeout=15) as response:
        return response.read().decode("utf-8")


def sky_state(description: str, layers: list[dict]) -> str:
    text = description.lower()
    if any(word in text for word in ("fog", "mist", "haze", "smoke")):
        return "fog"
    if any(word in text for word in ("overcast", "cloudy")):
        return "overcast"
    amounts = {layer.get("amount") for layer in layers}
    if "BKN" in amounts or "OVC" in amounts or "VV" in amounts:
        return "broken"
    if "SCT" in amounts or "FEW" in amounts:
        return "few"
    return "clear"


def precipitation_state(description: str) -> str:
    text = description.lower()
    if "thunder" in text:
        return "thunder"
    if any(word in text for word in ("rain", "shower", "drizzle")):
        return "rain"
    if any(word in text for word in ("snow", "sleet", "ice")):
        return "wintry"
    return "none"


def parse_mph(value: str | None) -> float | None:
    if not value:
        return None
    match = re.search(r"([0-9]+(?:\.[0-9]+)?)", value)
    return round(float(match.group(1)) * 0.868976, 1) if match else None


def ndbc_wave(url: str) -> dict:
    lines = [line for line in get_text(url).splitlines() if line and not line.startswith("#")]
    if not lines:
        raise ValueError("No realtime NDBC observations")
    values = lines[0].split()
    # NDBC standard columns: date fields, WDIR/WSPD/GST, WVHT, DPD, APD...
    def number(index: int) -> float | None:
        token = values[index] if index < len(values) else "MM"
        return None if token in ("MM", "99", "999") else float(token)
    return {
        "wave_height_m": number(8),
        "wave_period_s": number(9),
        "average_period_s": number(10),
        "mean_wave_direction_deg": number(11),
        "observed_at_utc": "-".join(values[:3]) + "T" + ":".join(values[3:5]) + ":00Z"
    }


def tide_state(config: dict, now: datetime) -> dict:
    url = config["ocean"]["tide_url_template"].format(
        begin_date=now.strftime("%Y%m%d"),
        end_date=(now + timedelta(days=1)).strftime("%Y%m%d")
    )
    data = get_json(url)
    predictions = data.get("predictions", [])
    if len(predictions) < 2:
        raise ValueError("No tide predictions")
    eastern = ZoneInfo(config["location"]["timezone"])
    events = []
    for prediction in predictions:
        event_time = datetime.strptime(prediction["t"], "%Y-%m-%d %H:%M").replace(tzinfo=eastern)
        events.append((event_time, prediction["type"], float(prediction["v"])))
    next_event = next((event for event in events if event[0] >= now), events[-1])
    prior = next((event for event in reversed(events) if event[0] < now), events[0])
    return {
        "phase": "flooding" if next_event[1] == "H" else "ebbing",
        "next_event": "high" if next_event[1] == "H" else "low",
        "next_event_at": next_event[0].isoformat(),
        "next_event_height_m": next_event[2],
        "previous_event": "high" if prior[1] == "H" else "low"
    }


def select_loop(result: dict) -> str | None:
    if not {"sky", "precipitation", "wave_height_m", "is_daytime"}.issubset(result):
        return None
    wave_height = result["wave_height_m"]
    sea = "calm" if wave_height < 0.6 else "moderate" if wave_height < 1.5 else "energetic"
    atmosphere = "rain" if result["precipitation"] != "none" else result["sky"]
    time = "day" if result["is_daytime"] else "night"
    return f"{time}-{atmosphere}-{sea}-sea-v1"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=Path("config/york-maine-conditions.json"))
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    now = datetime.now(ZoneInfo(config["location"]["timezone"]))
    result: dict = {
        "updated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "location": config["location"],
        "sources": {"observation": config["nws"]["preferred_observation_station"],
                    "wave_proxy": config["ocean"]["wave_station"],
                    "tide_proxy": config["ocean"]["tide_station"]},
        "degraded_sources": []
    }

    try:
        observation = get_json(
            f"https://api.weather.gov/stations/{config['nws']['preferred_observation_station']}/observations/latest"
        )["properties"]
        forecast = get_json(config["nws"]["hourly_forecast_url"])["properties"]["periods"][0]
        description = observation.get("textDescription", "")
        result.update({
            "observed_at": observation.get("timestamp"),
            "sky": sky_state(description, observation.get("cloudLayers", [])),
            "visibility_m": observation.get("visibility", {}).get("value"),
            "precipitation": precipitation_state(description),
            "forecast_precipitation_probability": forecast.get("probabilityOfPrecipitation", {}).get("value"),
            "wind_knots": (round(observation["windSpeed"]["value"] * 1.94384, 1)
                           if observation.get("windSpeed", {}).get("value") is not None else None),
            "wind_direction_deg": observation.get("windDirection", {}).get("value"),
            "temperature_c": observation.get("temperature", {}).get("value"),
            "forecast_summary": forecast.get("shortForecast"),
            "is_daytime": forecast.get("isDaytime")
        })
    except Exception as exc:
        result["degraded_sources"].append({"nws": str(exc)})

    try:
        result.update(ndbc_wave(config["ocean"]["wave_url"]))
    except Exception as exc:
        result["degraded_sources"].append({"wave": str(exc)})

    try:
        result["tide"] = tide_state(config, now)
    except Exception as exc:
        result["degraded_sources"].append({"tide": str(exc)})

    result["background_loop"] = select_loop(result)
    if result["degraded_sources"]:
        result["fallback"] = config["fallback"]
    print(json.dumps(result, indent=2 if args.pretty else None, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
