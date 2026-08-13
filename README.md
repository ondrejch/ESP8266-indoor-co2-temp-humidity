# ESP8266 Indoor CO2 / Temperature / Humidity Node

ESP8266 (Arduino IDE) node that samples an SCD41 (CO₂, temperature,
humidity) over I2C plus a DHT22 (temperature, humidity) and serves the
values as a [Prometheus](https://prometheus.io) text exposition endpoint
at `/metrics`, so Prometheus/Grafana can scrape the node directly.

Target board: Wemos D1 mini or NodeMCU (ESP8266 Arduino core 3.1.x).

## Features

- SCD41 in 5 s periodic measurement mode — CO₂, temperature, relative humidity
- DHT22 fallback channel for temperature/humidity (30000 ms non-blocking
  reads, tolerant of single glitches)
- Prometheus `/metrics` endpoint with HELP/TYPE metadata; invalid sensor
  samples are omitted instead of publishing `NaN`
- Also exports WiFi RSSI and uptime
- Non-blocking WiFi reconnect; HTTP server keeps running even when WiFi or
  sensors fail
- Onboard status LED: fast blink = WiFi down, slow pulse = connected/idle,
  solid flash = a `/metrics` scrape was just served

## Hardware

| Sensor pin      | ESP8266 board pin | GPIO   |
| --------------- | ----------------- | ------ |
| SCD41 VIN / 3V3 | 3V3               | —      |
| SCD41 GND       | GND               | —      |
| SCD41 SDA       | D2                | GPIO4  |
| SCD41 SCL       | D1                | GPIO5  |
| DHT VCC         | 3V3               | —      |
| DHT GND         | GND               | —      |
| DHT DATA        | D5                | GPIO14 |


⚠️  3.3 V logic only — do not power the SCD41 from 5 V. 

The DHT needs a 4.7–10 kΩ pull-up resistor from DATA to 3.3 V if it is the bare 4-pin sensor; 
most three-pin DHT breakout boards include it.

There have been reports of ESP8266 + SCD41 instability in some ESPHome/board combinations. 
If you encounter I²C timeouts or unexplained resets, use a direct Arduino/PlatformIO 
implementation with the Sensirion SCD4x library, reduce Wi‑Fi power-save aggressiveness, 
and make sure the board has a stable USB supply.

## Libraries

Install via Arduino Library Manager:

- Adafruit DHT sensor library 
- Sensirion I2C SCD4x **1.1.x** (header `SensirionI2cScd4x.h` — older 0.4.x
  names will not compile)

## Configuration

Edit `esp-co2-dht/esp-co2-dht.h` and fill in your own values before flashing:

```c
#define SECRET_WIFI_SSID "your-ssid"
#define SECRET_WIFI_PASS "your-password"
#define MY_LOCATION "room1"     // appears as location="..." label in /metrics
#define MY_HOSTNAME "co2"       // DHCP hostname; used as a hint in logs
```

The example header ships with placeholder SSID/password — never publish real
credentials if you push this repository anywhere public.

## Usage

1. Open `esp-co2-dht/esp-co2-dht.ino` in the Arduino IDE.
2. Board: `LOLIN(WEMOS) D1 mini lite` or `NodeMCU 1.0 (ESP-12E)`; CPU freq
   80 MHz is fine.
3. Upload. Open Serial Monitor at 115200 baud.
4. The node serves plain text at `http://<ip>/` and Prometheus metrics at
   `http://<ip>/metrics`.

Example scrape (Prometheus 0.0.4):

```text
# HELP co2_ppm Carbon dioxide concentration in parts per million
# TYPE co2_ppm gauge
esp8266_co2_ppm{sensor="scd41",location="room1"} 612

# HELP dht_temperature_c Temperature measured by DHT sensor in Celsius
# TYPE dht_temperature_c gauge
esp8266_dht_temperature_c{location="room1"} 22.31
...
```

## Prometheus metrics

| Metric | Type | Source |
|---|---|---|
| `esp8266_co2_ppm{sensor="scd41"}` | gauge | SCD41 |
| `esp8266_scd41_temperature_c` | gauge | SCD41 |
| `esp8266_scd41_relative_humidity_percent` | gauge | SCD41 |
| `esp8266_dht_temperature_c` | gauge | DHT22 |
| `esp8266_dht_relative_humidity_percent` | gauge | DHT22 |
| `esp8266_wifi_rssi_dbm` | gauge | WiFi |
| `esp8266_uptime_seconds` | counter | uptime |

All metrics carry the `location="..."` label from `MY_LOCATION`.

## Implementation notes

- SCD41 errors trigger a 5 s I2C backoff before retrying; the first periodic
  sample after start can be 0/invalid and is discarded, keeping the previous
  published values.
- DHT22 is slow and blocking (~250 ms, interrupts off); reads happen at most
  every 30 s so bit-bang does not collide with I2C/WiFi.
- Modem sleep is disabled (`WiFi.setSleep(false)`) — a common NACK/reset
  combo with DHT `noInterrupts()` on always-on scraped nodes. This board is
  USB-powered, so it stays awake.

## License

MIT — see [LICENSE](LICENSE). © 2026 Ondrej Chvala.
