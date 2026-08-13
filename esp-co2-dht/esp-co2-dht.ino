// ESP8266 indoor-air node: SCD41 (CO2 + T + RH) and a DHT22 (T + RH).
// Serves Prometheus text on :80/metrics so Prometheus/Grafana can scrape it
// directly. Board: Wemos D1 mini or NodeMCU (ESP8266 Arduino 3.1.x).
//
// Wiring (3.3 V only — do not power the SCD41 from 5 V):
//   SCD41 SDA -> D2 (GPIO4)    SCD41 SCL -> D1 (GPIO5)
//   DHT data  -> D5 (GPIO14)   DHT needs its usual 4.7k–10k pull-up to 3.3 V
//                              DHT22 in a 3-pin breakout/module already has one
//   Status LED: onboard LED_BUILTIN (D4 / GPIO2, active-low on Wemos/NodeMCU)
//     fast blink  = WiFi down
//     slow pulse  = WiFi up, idle
//     solid flash = /metrics scrape just served
//
// Libraries (Library Manager): DHT sensor library, Sensirion I2C SCD4x 1.1.x
// (header SensirionI2cScd4x.h — older 0.4.x names will not compile).
//
// Ondrej Chvala <ondrejch@gmail.com>
// MIT license

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <DHT.h>
#include <SensirionI2cScd4x.h>

#include "esp-co2-dht.h" 

const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;
const char* LOCATION  = MY_LOCATION;
const char* HOSTNAME  = MY_HOSTNAME;  // DHCP hostname; also used as a hint in logs

// ---- Wemos D1 mini / NodeMCU pins ----
constexpr uint8_t I2C_SDA = D2;  // GPIO4, default ESP8266 I2C data
constexpr uint8_t I2C_SCL = D1;  // GPIO5, default ESP8266 I2C clock
constexpr uint8_t DHT_PIN = D5;  // GPIO14, one-wire DHT data
constexpr uint8_t LED_PIN = LED_BUILTIN;
constexpr bool LED_ACTIVE_LOW = true;  // onboard LED lights when the pin is LOW

// Change DHT22 to DHT11 if appropriate.
#define DHT_TYPE DHT22

// DHT22 is slow and blocking (~250 ms, interrupts off). 30 s is well above
// its 2 s minimum and keeps bit-bang from colliding with I2C / WiFi often.
constexpr uint32_t DHT_INTERVAL_MS = 30000;
// A single DHT miss is common on ESP8266; drop the series only after repeats
// so Prometheus does not insert a gap on every glitch.
constexpr uint8_t DHT_FAIL_LIMIT = 3;

// SCD41 periodic mode updates every 5 s. Polling ready-status at 1 s is
// enough; faster just hammers I2C. After a bus error, back off before retry.
constexpr uint32_t SCD_POLL_MS = 1000;
constexpr uint32_t SCD_ERROR_BACKOFF_MS = 5000;

// First join may block up to this long, then we start HTTP anyway so a later
// reconnect can still serve /metrics. Background retries stay non-blocking.
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t WIFI_RETRY_MS = 15000;

constexpr uint32_t LED_TX_MS = 250;         // hold on after a /metrics send
constexpr uint32_t LED_HB_ON_MS = 80;       // connected heartbeat pulse
constexpr uint32_t LED_HB_PERIOD_MS = 2000;
constexpr uint32_t LED_SEARCH_HALF_MS = 150;  // disconnected blink half-period

ESP8266WebServer server(80);
DHT dht(DHT_PIN, DHT_TYPE);
SensirionI2cScd4x scd4x;

// Last good samples. Exported only while *Valid is true.
uint16_t co2ppm = 0;
float scdTempC = NAN;
float scdHumidity = NAN;
float dhtTempC = NAN;
float dhtHumidity = NAN;

bool scdValid = false;
bool dhtValid = false;
uint8_t dhtFailCount = 0;

uint32_t lastDhtReadMs = 0;
uint32_t lastScdPollMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t scdBackoffUntilMs = 0;
uint32_t ledTxUntilMs = 0;

void setLed(bool on) {
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? !on : on);
}

void signalTx() {
  ledTxUntilMs = millis() + LED_TX_MS;
  setLed(true);
}

void updateLed() {
  const uint32_t now = millis();
  if ((int32_t)(ledTxUntilMs - now) > 0) {
    setLed(true);
    return;
  }

  const bool connected = (WiFi.status() == WL_CONNECTED);
  const uint32_t onMs = connected ? LED_HB_ON_MS : LED_SEARCH_HALF_MS;
  const uint32_t periodMs = connected ? LED_HB_PERIOD_MS : (2 * LED_SEARCH_HALF_MS);
  setLed((now % periodMs) < onMs);
}

void logSensirionError(const char* what, int16_t error) {
  char errorMessage[128];
  errorToString(error, errorMessage, sizeof(errorMessage));
  Serial.printf("%s: %s\n", what, errorMessage);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  // Modem sleep + DHT noInterrupts() + SCD41 I2C is a common NACK/reset combo
  // on always-on scraped nodes. This board is USB-powered; stay awake.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("Connecting to %s\n", WIFI_SSID);
  const uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
    updateLed();
    delay(10);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect timed out; will retry in background");
  }
}

// Kick the STA again if the AP dropped. Do not call disconnect() — the
// one-arg form erases saved credentials and this must not block handleClient().
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetryMs < WIFI_RETRY_MS) return;
  lastWifiRetryMs = millis();
  Serial.println("WiFi reconnecting");
  WiFi.reconnect();
}

void readScd41() {
  const uint32_t now = millis();
  if ((int32_t)(scdBackoffUntilMs - now) > 0) return;
  if (now - lastScdPollMs < SCD_POLL_MS) return;
  lastScdPollMs = now;

  // dataReady is the sensor's 5 s "new sample" bit, not "last reading is good".
  // Reusing scdValid here would blank /metrics for ~5 s minus one loop.
  bool dataReady = false;
  int16_t error = scd4x.getDataReadyStatus(dataReady);
  if (error != 0) {
    logSensirionError("SCD41 data-ready error", error);
    scdValid = false;
    scdBackoffUntilMs = now + SCD_ERROR_BACKOFF_MS;
    return;
  }

  if (!dataReady) return;

  uint16_t co2 = 0;
  float temperature = NAN;
  float humidity = NAN;
  error = scd4x.readMeasurement(co2, temperature, humidity);
  if (error != 0) {
    logSensirionError("SCD41 read error", error);
    scdValid = false;
    scdBackoffUntilMs = now + SCD_ERROR_BACKOFF_MS;
    return;
  }

  // Datasheet: first periodic sample after start can be 0 / invalid.
  // Keep the previous published values if we already have some.
  if (co2 == 0) return;

  co2ppm = co2;
  scdTempC = temperature;
  scdHumidity = humidity;
  scdValid = true;
}

void readDht() {
  if (millis() - lastDhtReadMs < DHT_INTERVAL_MS) return;
  lastDhtReadMs = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.println("DHT read failed");
    if (dhtFailCount < 255) dhtFailCount++;
    if (dhtFailCount >= DHT_FAIL_LIMIT) dhtValid = false;
    return;
  }

  dhtTempC = t;
  dhtHumidity = h;
  dhtValid = true;
  dhtFailCount = 0;
}

String locationLabels() {
  return String("location=\"") + LOCATION + "\"";
}

// Prometheus 0.0.4 text exposition. HELP/TYPE are always emitted; the sample
// line is omitted when the sensor is invalid so scrapes do not publish NaN.
void metricsHandler() {
  String out;
  out.reserve(1800);

  out += "# HELP co2_ppm Carbon dioxide concentration in parts per million\n";
  out += "# TYPE co2_ppm gauge\n";
  if (scdValid) {
    out += "esp8266_co2_ppm{sensor=\"scd41\",";
    out += locationLabels();
    out += "} ";
    out += String(co2ppm);
    out += "\n";
  }
  out += "# HELP scd41_temperature_c Temperature measured by the SCD41 in Celsius\n";
  out += "# TYPE scd41_temperature_c gauge\n";
  if (scdValid) {
    out += "esp8266_scd41_temperature_c{";
    out += locationLabels();
    out += "} ";
    out += String(scdTempC, 2);
    out += "\n";
  }
  out += "# HELP scd41_relative_humidity_percent Relative humidity measured by the SCD41\n";
  out += "# TYPE scd41_relative_humidity_percent gauge\n";
  if (scdValid) {
    out += "esp8266_scd41_relative_humidity_percent{";
    out += locationLabels();
    out += "} ";
    out += String(scdHumidity, 2);
    out += "\n";
  }
  
  out += "# HELP dht_temperature_c Temperature measured by DHT sensor in Celsius\n";
  out += "# TYPE dht_temperature_c gauge\n";
  if (dhtValid) {
    out += "esp8266_dht_temperature_c{";
    out += locationLabels();
    out += "} ";
    out += String(dhtTempC, 2);
    out += "\n";
  }
  out += "# HELP dht_relative_humidity_percent Relative humidity measured by DHT sensor\n";
  out += "# TYPE dht_relative_humidity_percent gauge\n";
  if (dhtValid) {
    out += "esp8266_dht_relative_humidity_percent{";
    out += locationLabels();
    out += "} ";
    out += String(dhtHumidity, 2);
    out += "\n";
  }

  out += "# HELP esp8266_wifi_rssi_dbm WiFi received signal strength\n";
  out += "# TYPE esp8266_wifi_rssi_dbm gauge\n";
  out += "esp8266_wifi_rssi_dbm{";
  out += locationLabels();
  out += "} ";
  out += String(WiFi.RSSI());
  out += "\n";

  out += "# HELP esp8266_uptime_seconds Device uptime in seconds\n";
  out += "# TYPE esp8266_uptime_seconds counter\n";
  out += "esp8266_uptime_seconds{";
  out += locationLabels();
  out += "} ";
  out += String(millis() / 1000);
  out += "\n";


  server.send(200, "text/plain; version=0.0.4; charset=utf-8", out);
  signalTx();
}

void rootHandler() {
  String page = "ESP8266 CO2 monitor\n";
  page += "Prometheus endpoint: /metrics\n";
  page += "IP: " + WiFi.localIP().toString() + "\n";
  server.send(200, "text/plain", page);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  setLed(false);
  delay(250);

  dht.begin();

  Wire.begin(I2C_SDA, I2C_SCL);
  scd4x.begin(Wire, SCD41_I2C_ADDR_62);

  // Idle the sensor first: a leftover periodic session from a previous
  // flash/reset NACKs startPeriodicMeasurement() until stop completes.
  int16_t error = scd4x.stopPeriodicMeasurement();
  if (error != 0) {
    logSensirionError("SCD41 stop error", error);
  }

  delay(500);  // stopPeriodicMeasurement needs ~500 ms before the next command

  // 5 s update interval. Low-power periodic (30 s) exists but is not used here.
  error = scd4x.startPeriodicMeasurement();
  if (error != 0) {
    logSensirionError("SCD41 start error", error);
  }

  connectWiFi();

  server.on("/", HTTP_GET, rootHandler);
  server.on("/metrics", HTTP_GET, metricsHandler);
  server.begin();

  Serial.println("HTTP server started");
}

void loop() {
  maintainWiFi();
  server.handleClient();  // must keep running even when WiFi or sensors fail
  readScd41();
  readDht();
  updateLed();
  delay(10);  // yield to the ESP8266 WiFi stack
}
