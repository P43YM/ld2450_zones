# ld2450_zones

<img width="1463" height="1163" alt="web" src="https://github.com/user-attachments/assets/d2c08142-effd-4e66-8e83-87c9f0df8bda" />

Smart presence zones for the **HLK-LD2450** mmWave radar, running right on your ESP32.

An ESPHome component that turns the LD2450 into a set of presence zones, with all the logic on the ESP. The ESP reads every radar frame (about 10 per second), filters out the noise, checks where each target is standing against **zones of any shape**, and tells Home Assistant only when something actually changes. You draw the zones in a web editor served by the ESP itself, with a live view of the targets walking around. You can draw multiple segments in one zone, and overlap them with other zones. Simple and effective. No additional scripts and graphs needed.

Runs on ESP32 boards with the ESP-IDF framework,example is built for the ESP32-C3 Super Mini.

## What you get

- **Full-rate processing on the device.** Nothing is dropped or throttled before it gets evaluated, and Home Assistant isn't flooded with coordinates.
- **Zones of any shape.** L-shaped, with holes, in several pieces: a zone is just a set of grid cells (30 x 30 cells of 20 cm by default), so shape doesn't cost anything.
- **A proper web editor.** Paint cells with a brush, drag rectangles, draw polygons. Or stand in a corner of the room and press a button to drop a polygon vertex right where you're standing.
- **Presence that doesn't flicker.** A zone turns on when a target shows up in it often enough, and stays on for as long as you tell it to after the last sighting. Both are adjustable on the fly.
- **An ignore zone** for fans, curtains, plants and anything else that keeps producing phantom targets.
- **Built-in diagnostics.** Frame counters, gaps between frames, loop delay, per-target status and a timestamped event log, so you can see *why* something happened.
- **Everything is remembered.** Shapes, hold times and filter settings live in flash and survive reboots.
- **English and Russian interface**, switchable in the header.

## Wiring

| LD2450 | ESP32-C3 Super Mini |
|--------|---------------------|
| 5V     | 5V                  |
| GND    | GND                 |
| TX     | GPIO20 (RX)         |
| RX     | GPIO21 (TX)         |


## Installation

Add the component to your ESPHome config:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/P43YM/ld2450_zones
      ref: v1.0.0
    components: [ ld2450_zones ]
```

Or the short form:

```yaml
external_components:
  - source: github://P43YM/ld2450_zones@v1.0.0
    components: [ ld2450_zones ]
```

Pinning a tag keeps your builds reproducible. If you'd rather track a branch, use its name as `ref` and add `refresh: 1d` so ESPHome re-checks it once a day.

Working from a local copy? Point `path` at the folder that contains `ld2450_zones/`:

```yaml
external_components:
  - source:
      type: local
      path: components
```

## A minimal setup

```yaml
esphome:
  name: ld2450-zones

esp32:
  board: esp32-c3-devkitm-1
  framework:
    type: esp-idf

logger:
  hardware_uart: USB_SERIAL_JTAG   # frees GPIO20/21 for the radar

api:
ota:
  - platform: esphome
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

uart:
  id: radar_uart
  tx_pin: GPIO21
  rx_pin: GPIO20
  baud_rate: 256000                # the LD2450 only speaks this
  rx_buffer_size: 512

ld2450_zones:
  uart_id: radar_uart
  presence:
    name: "Radar presence"
  zones:
    - label: "Sofa"
      presence:
        name: "Zone Sofa"
    - label: "Desk"
      presence:
        name: "Zone Desk"
```

[`example.yaml`](example.yaml) has a fuller version with counters and name sensors.

Flash it, open `http://<device-ip>/`, and start drawing.

## Configuration

### `ld2450_zones`

| Option | Default | What it does |
|--------|---------|--------------|
| `uart_id` | required | The UART wired to the radar (256000 baud, both pins). |
| `web_port` | `80` | Port of the web editor. |
| `multi_target` | `true` | Switches the radar to multi-target tracking a few seconds after boot. Set to `false` if you'd rather manage that yourself. |
| `area_width` | `6000` | Width of the zone field in mm, centred on the radar (1000 to 10000). |
| `area_depth` | `6000` | Depth of the zone field in mm, going forward from the radar (1000 to 8000). |
| `cell_size` | `200` | Grid cell size in mm (50 to 1000). Width and depth must divide by it evenly, and the grid can have at most 1024 cells. |
| `enter_frames` | `3` | A zone turns on when a target was seen in it at least this many times... |
| `enter_window` | `10` | ...within the last this many frames (10 frames is roughly a second). Can't be smaller than `enter_frames`. |
| `hold` | `5s` | How long the **overall** presence stays on after the last counted target. |
| `presence` | none | Binary sensor: any counted target anywhere in the field. |
| `target_count` | none | Sensor: how many targets are counted right now (0 to 3). Updates at most once a second. |
| `zones` | `[]` | Up to 8 zone slots (see below). |

`enter_frames`, `enter_window` and `hold` are starting values. Once you press **Apply filter** in the web UI, the values saved on the device win over the YAML.

### `zones[]`

| Option | Default | What it does |
|--------|---------|--------------|
| `label` | required | The zone's starting name. Rename it later in the UI. |
| `hold` | `5s` | How long the zone stays occupied after the last target left. Editable in the UI. |
| `presence` | none | Occupancy binary sensor for the zone. |
| `count` | none | Sensor: how many targets are inside the zone right now. |
| `label_sensor` | none | Text sensor with the zone's current name (see [Zone names in Home Assistant](#zone-names-in-home-assistant)). |

A zone's *shape* isn't in the YAML: you draw it in the editor. The number of zone *slots* is fixed when you build the firmware, because Home Assistant entities are static.

## The web editor

Open `http://<device-ip>:8080/`.

- **The field.** The radar sits at the bottom centre and the dashed wedge is its field of view (about 60 degrees to each side). Metre lines run over the cell grid, and targets show up with little trails behind them.
- **Tools.** Brush (1x1, 3x3 or 5x5), Rectangle and Polygon, each in Add or Erase mode.
- **Snap to target.** In Polygon or Rectangle mode, **Vertex at target** (key `V`) puts a point exactly where the target is standing. Walk to a corner, press it, walk to the next one. It's the fastest way to trace a sofa or a desk.
- **Shortcuts.** `V` adds a vertex at the target, `Enter` fills the polygon, `Backspace` removes the last vertex, `Esc` starts over. Clicking near the first vertex closes the outline too.
- **Saving.** Changes stay in your browser until you press **Save zone** or **Save all**. A dot marks zones with unsaved edits.
- **Ignore zone.** The last item in the zone list. Anything standing in the painted cells is dropped completely, for zones and overall presence alike.
- **Filtering.** Set "seen N times in the last M frames" and the overall hold time. The little `3/10` badge next to a zone shows how many frames in the current window contained a target.
- **Diagnostics.** Firmware build, frames received, bad frames, skipped bytes, the longest gap between frames, the longest ESP loop delay, targets seen versus counted, and an event log with timestamps.
- **Try it without hardware.** Open `ui/ui.html?demo` in any browser for a simulated radar.

## How presence works

For every frame, each of the (up to three) targets is mapped to a grid cell. Targets outside the field, or inside the ignore zone, are not counted at all.

1. **Turning on.** A zone (or the overall presence) switches on once a target has been seen in it at least `enter_frames` times in the last `enter_window` frames. A single noisy frame won't do it; a target that keeps flickering in and out will.
2. **Staying on.** While it's on, *any* sighting restarts the hold timer. A motionless person the radar loses for a few frames won't switch the light off.
3. **Turning off.** Once nothing has been seen in the zone for the hold time, it switches off.
4. **If the radar goes quiet.** With no frames for 2 seconds, empty frames are fed into the logic so zones still expire on time.

Home Assistant is updated only when a binary sensor actually changes. The `count` and `target_count` sensors are also limited to one update per second.

## Zone names in Home Assistant

Entity names such as the zone's `presence` sensor are baked into the firmware from your YAML, so renaming a zone in the web editor doesn't rename those entities. To get the current name into Home Assistant, add a `label_sensor`:

```yaml
zones:
  - label: "Sofa"
    presence: { name: "Zone Sofa" }
    label_sensor: { name: "Zone Sofa name" }
```

It publishes the name at boot and after every rename, no reflashing needed. If you want to rename the entities themselves, do it in Home Assistant's entity settings (that survives firmware updates) or change the names in the YAML and update the device.

## HTTP API

The editor talks to the device through a small JSON API, and you're welcome to script against it. There's no authentication, so keep the device on a network you trust.

| Endpoint | What it does |
|----------|--------------|
| `GET /` | The web editor. |
| `GET /api/state` | Live state: targets, per-zone occupancy and hit counters, diagnostics. The UI polls it about ten times a second. |
| `GET /api/zones` | Grid geometry, zone names and hold times, cell bitmaps (strings of `0` and `1`), the ignore mask and the filter settings. |
| `POST /api/zone` | Saves a zone. Body: `<index>,<hold seconds>\n<name>\n<cells>`. Index `255` is the ignore mask. `<cells>` is one `0` or `1` per cell, row by row starting next to the radar. |
| `POST /api/settings` | Saves the filter. Body: `<enter_frames>,<enter_window>,<hold seconds>`. |

## Good to know about the radar

- Coordinates are in millimetres: `x` is the sideways offset from the radar's axis, `y` is the distance straight ahead. Speed is in cm/s.
- The LD2450 tracks up to **3 targets**, sends about **10 frames a second**, and sees roughly 6 m ahead within about 60 degrees to each side.
- People who sit perfectly still tend to drop out of the radar's tracking for a few frames at a time. That's the sensor, not a bug, and it's exactly what the sliding-window filter and the hold time are for. If a zone still flickers, give it a longer hold.
- Saved zones are tied to the grid. If changing `area_width`, `area_depth` or `cell_size` changes the number of columns or rows, the saved zones are discarded and you'll need to redraw them. If the cell counts stay the same, your shapes are kept and just follow the new cell size.

## Troubleshooting

| What you see | What to try |
|--------------|-------------|
| `Component not found: ld2450_zones` | The repository has to contain `components/ld2450_zones/`, and the folder name has to match the component name. |
| `Component ld2450_zones cannot be loaded via YAML (no CONFIG_SCHEMA)` | `__init__.py` is missing, empty or misnamed (two underscores on each side). |
| The editor won't open | Check `web_port`, make sure nothing else is using it, and that the device is on Wi-Fi. |
| Presence flickers | Open **Diagnostics** and read the event log. "OFF" should show up only about *hold* seconds after the last counted target. If *targets seen / counted* differ, targets are outside the field or in the ignore zone. If the radar simply loses a still person for a long time, raise the hold. |
| Targets vanish and come back, with big gaps between frames | Almost always power. Use a decent 5 V supply and a short, thick cable. |
| Diagnostics says the firmware is an old build | The board is running firmware from before the diagnostics existed. Rebuild and flash. |

## Development

The whole web interface is one file, [`ui/ui.html`](ui/ui.html), embedded into the firmware as a C++ string in `components/ld2450_zones/ui.h`. After editing the HTML, regenerate the header:

```bash
python3 gen_ui_h.py
```

Commit the regenerated `ui.h`: ESPHome builds from it and never reads `ui.html`.

The presence logic (frame parser, filter, zone lookup) is plain C++ with no hardware dependencies, so it's easy to test on a PC. One rule if you touch anything that deals with time: `millis()` values are unsigned and wrap around, so compare them with the `elapsed_ms()` helper instead of subtracting them directly.

Pull requests and bug reports are welcome. When reporting a problem, a screenshot of the Diagnostics panel with the event log helps a lot.

## Release notes

The build ID shown in Diagnostics tells you which of these you're running.

- **v2.3** English and Russian interface; optional `label_sensor` for zone names.
- **v2.2** Fixed a timing bug that made targets blink and dropped presence right after a detection. If you're on an older build and see flicker, update.
- **v2.1** Diagnostics panel and event log.
- **v2.0** Sliding-window filter, ignore zone, editable zone names, filter settings changeable at runtime.

## License

MIT. See [LICENSE](LICENSE).
