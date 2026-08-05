# Luminary P4 runtime viewer

Production ESP-IDF firmware for the **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B** only. It drives the 1024×600 EK79007 MIPI-DSI panel in RGB888 and renders a continuously evolving Nubble sea and sky behind the printed bas-relief.

## Registration contract

- Canvas: exactly 1024×600
- Camera bearing: 90°
- Horizon: immutable row **291**
- The packed water mask is false above row 291.
- Cloud opacity is feathered to zero before the horizon and cannot touch the sea.
- Foam can exist only in registered water pixels near the signed shore-distance field.
- Land is physical printed relief; the runtime never invents or moves it.

## Runtime model

The base image is decoded once by the P4 JPEG engine. At 6 fps, firmware then:

1. Projects a two-component measured swell through the locked view without moving the horizon.
2. Restricts deformation and shore foam to the registered water mask.
3. Composites GOES high, mid, and low cloud shells, each rotated continuously by its own GOES Derived Motion Wind vector and measured cloud height.
4. Uses the live Nubble camera as the final authority for visible sky color and cloud fraction. A confident clear camera observation sets shell opacity to zero even if an older satellite field contains cloud.
5. Synchronizes UTC directly with SNTP and recalculates York's actual solar altitude and azimuth on the P4 every 30 seconds. Golden hour begins continuously at +10°, peaks at the horizon crossing, and fades through twilight. Sunset remains physically behind the east-facing camera while warming the correctly oriented horizon, clouds, and water reflection. A moon is drawn only when measured altitude/azimuth places it inside the camera view, before cloud compositing so clouds can occlude it.
6. Projects the ESA Hipparcos naked-eye catalogue through the same locked camera. ICRS positions receive catalogue proper motion and date precession before conversion to York-local altitude/azimuth. Solar altitude controls limiting magnitude, and the three measured cloud shells attenuate stars rather than allowing them to shine through cloud.

The compiled asset is an offline-safe fallback. In production the P4 polls a versioned HTTPS manifest every five minutes, downloads changed state, three compact 256×96 luminance/alpha shell atlases, and the projected ocean phase field, verifies every size and CRC32, then activates the complete bundle atomically. It keeps rendering the previous validated bundle through network or upstream failures.

For visual QA on the local network, `GET /runtime/screenshot.ppm` returns a lossless capture of the exact BGR888 scanout framebuffer converted to RGB PPM. This captures only display content; the physical Nubble bas-relief remains intentionally absent.

The default endpoint is the repository's force-refreshed `runtime-live` branch on `raw.githubusercontent.com`, which has a valid public TLS chain. `luminary.castalia.institute` mirrors the project site but is not trusted as a firmware endpoint until its custom-domain certificate is valid.

## Autonomous refresh

The P4 initiates refreshes; it does not require an inbound connection or a Mac on the local network. GitHub Actions preprocesses the heavy NOAA GOES NetCDF products and Nubble live-camera observation into the compact bundle the P4 can consume. The workflow replaces the history-free `runtime-live` branch every five minutes, avoiding unbounded binary Git history.

The scene swap is all-or-nothing:

1. Fetch `manifest.json` over verified HTTPS.
2. Ignore an unchanged `bundle_id`.
3. Download all cache-busted binary assets into temporary PSRAM.
4. Validate byte counts, CRC32 values, state JSON, and immutable horizon row 291.
5. Acquire the renderer lock once and activate the complete bundle.

The cloud and sea fields continue moving from monotonic time while a refresh is downloaded.

## Optional microSD asset cache

Insert a FAT-formatted microSD card in the onboard TF socket to retain the last validated runtime scene across power loss and offline starts. The cache is optional: firmware always boots from its embedded scene when the card is absent, unreadable, or corrupt, and then refreshes over HTTPS when Wi-Fi becomes available.

The P4 stores two alternating files under `/luminary/runtime-{a,b}.bundle`. Each file contains the state JSON, all three cloud atlases, and the ocean phase field with a sequence number and CRC32. The validity marker is written only after the complete file has been flushed, so a power interruption during an update leaves the other slot intact. At boot, firmware validates both slots and activates the newest complete bundle before connecting to the network.

The TF socket runs in its board-supported SPI mode on MISO 39, CS 42, CLK 43, and MOSI 44. This deliberately isolates removable storage from the ESP-C6 Wi-Fi coprocessor, which uses the P4's separate SDIO slot. The fixed Hipparcos catalogue and boot-safe scene remain embedded in flash.

## Maintenance push API

The P4 still listens on port 80 after Wi-Fi connects for development and recovery:

- `POST /runtime/cloud/high`
- `POST /runtime/cloud/mid`
- `POST /runtime/cloud/low`
- `POST /runtime/ocean-phase`
- `POST /runtime/state`

Each cloud payload is exactly 49,152 bytes and the ocean phase payload is exactly 614,400 bytes. The host uploads all binary fields first and the JSON state last; the state request is the visible commit point. The device rejects any state whose horizon is not row 291.

From the repository root:

```sh
python3 scripts/update-york-runtime.py --deploy
```

That command fetches York weather, buoy and tide data, observes the live Nubble camera, downloads GOES C13/ACHA clouds and C14 motion winds, builds and validates the three shells, packs the runtime assets, then atomically deploys them to the P4. Use `--reuse-dmw` only for offline/repeat testing.

## Optional local builder

The macOS LaunchAgent is an optional development fallback, not part of the production runtime path. It can build and push a scene directly while testing upstream changes. Production data is generated by `.github/workflows/runtime.yml`, and the P4 fetches it itself.

Service label: `institute.castalia.luminary-realtime`

```sh
launchctl print gui/$(id -u)/institute.castalia.luminary-realtime
tail -f ~/Library/Logs/Luminary/realtime.log
```

The host Mac needs to be awake and connected to the P4's local network only when using this optional direct-push path.

Validate and preview without deploying:

```sh
python3 scripts/update-york-runtime.py --reuse-dmw
python3 scripts/preview-p4-runtime.py \
  --state firmware/luminary-background-viewer/assets/v2/nubble-runtime-scene-v1.json \
  --assets firmware/luminary-background-viewer/assets/runtime \
  --output tmp/york-live/runtime-production-preview
python3 scripts/validate-production-runtime.py \
  --state firmware/luminary-background-viewer/assets/v2/nubble-runtime-scene-v1.json \
  --assets firmware/luminary-background-viewer/assets/runtime \
  --preview tmp/york-live/runtime-production-preview
```

## Build and flash

Wi-Fi credentials remain local build inputs in ignored `sdkconfig`; do not put them in source or logs.

```sh
. /Users/danielmcshan/esp/esp-idf-v5.5.1/export.sh
idf.py -B build-runtime set-target esp32p4
idf.py -B build-runtime build
idf.py -B build-runtime -p /dev/cu.wchusbserial5B901846451 flash monitor
```

Only use a serial path after positively identifying this exact P4 board. The current application is about 2.87 MB, leaving roughly 9% of the 3 MB app partition free. Device measurements are approximately 152 ms/frame for clear sea/sky and 284 ms/frame with all three shell samplers active. Firmware therefore uses 6 fps when clear and 3 fps when cloud shells are visible, keeping both modes inside their cadence budgets.

Legacy `.lumv` assets remain under `assets/v2` for comparison, but the production firmware no longer loops finite JPEG sequences.
