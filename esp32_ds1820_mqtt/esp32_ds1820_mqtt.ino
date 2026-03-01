/*
    esp8266 ds18x20 MQTT (on esp32)

    See https://github.com/oh2mp/

    All kind of butchery from porting to esp32 by oh8th
    
    FIXES APPLIED:
    - Buffer overflow protection in getSensorIndex()
    - NaN comparison using isnan()
    - Null-termination after file reads
    - Length checking before buffer access
    - Proper array initialization
    - Safe index bounds checking
    - Dynamic string handling for large data
*/

#include <OneWire.h>
#include <DallasTemperature.h>

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <cmath>

#define FORMAT_SPIFFS_IF_FAILED true

/* ------------------------------------------------------------------------------- */
//      Name   GPIO    Function */
#define PIN_D3 0  // Low on boot means enter FLASH mode
#define PIN_D4 4  //
/* ------------------------------------------------------------------------------- */

int LED = 2;

#define LED_ON LOW  // Reverse these if you use eg. ESP12E, no NodeMCU dev module.
#define LED_OFF HIGH

#define ONE_WIRE_BUS PIN_D4
#define APREQUEST PIN_D3
#define APTIMEOUT 120000
#define MAX_SENSORS 10
#define MAX_SENSOR_ROWS 32


float sens[MAX_SENSORS];         // sensor values
char sensname[MAX_SENSORS][24];  // sensor names
float senscal[MAX_SENSORS];      // sensor calibration offsets in Celsius
int sread = 0;                   // Flag if sensors have been read on this iteration
int onewire_wait = 1;            // if we are waiting for 1wire data
unsigned long mytime = 0;        // Used for delaying, see loop function
int scount = 0;                  // sensors amount
int interval = 0;                // interval in minutes
volatile unsigned long portal_timer = 0;

// Default hostname base. Last 3 octets of MAC are added as hex.
// The hostname can be changed explicitly from the portal.
// Maxlen in ESP8266WiFi class is 24
char myhostname[128] = "esp32-ds1820-";

// placeholder values
char topicbase[256] = "dallastemp";
char mqtt_user[128] = "foo";
char mqtt_pass[128] = "bar";
char mqtt_host[64] = "192.168.202.9";
int mqtt_port = 1883;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress sensor[MAX_SENSORS];

// ESP8266WiFiMulti WiFiMulti;
WiFiClient wificlient;
PubSubClient mqttclient(wificlient);

WebServer server(80);
IPAddress apIP(192, 168, 4, 1);  // portal ip address
File file;
TaskHandle_t sensorTaskHandle = nullptr;

void sensorMqttTask(void *parameter);

volatile bool calibrationActive = false;
volatile bool calibrationCompleted = false;
volatile bool calibrationTimedOut = false;
volatile unsigned long calibrationStartMs = 0;
volatile unsigned long calibrationLastSampleMs = 0;
volatile int calibrationSelectedCount = 0;
volatile int calibrationDoneCount = 0;
bool calibrationSelected[MAX_SENSORS] = { false };
bool calibrationReady[MAX_SENSORS] = { false };
bool calibrationDone[MAX_SENSORS] = { false };
bool calibrationHasPrev[MAX_SENSORS] = { false };
float calibrationPrevTemp[MAX_SENSORS] = { 0 };
float calibrationLastTemp[MAX_SENSORS] = { NAN };
float calibrationSampleSum[MAX_SENSORS] = { 0 };
uint8_t calibrationSampleCount[MAX_SENSORS] = { 0 };
float calibrationComputedOffset[MAX_SENSORS] = { 0 };

int calibrationRowCount = 0;
char calibrationRowAddr[MAX_SENSOR_ROWS][17];
char calibrationRowName[MAX_SENSOR_ROWS][25];
float calibrationRowCal[MAX_SENSOR_ROWS];
int calibrationRowSensorIdx[MAX_SENSOR_ROWS];

const float CALIBRATION_READY_RANGE_C = 2.0;
const float CALIBRATION_STABLE_DELTA_C = 0.25;
const uint8_t CALIBRATION_TARGET_SAMPLES = 10;
const unsigned long CALIBRATION_SAMPLE_INTERVAL_MS = 1000;
const unsigned long CALIBRATION_TIMEOUT_MS = 5UL * 60UL * 1000UL;

/* ------------------------------------------------------------------------------- */
void loadWifis() {
  if (SPIFFS.exists("/known_wifis.txt")) {
    char ssid[128];
    char pass[128];

    File file = SPIFFS.open("/known_wifis.txt");
    if (!file) {
      Serial.println("Failed to open known_wifis.txt");
      return;
    }

    while (file.available()) {
      memset(ssid, 0, sizeof(ssid));
      memset(pass, 0, sizeof(pass));

      // FIX: Add null-termination after readBytesUntil
      int ssid_len = file.readBytesUntil('\t', ssid, sizeof(ssid) - 1);
      ssid[ssid_len] = '\0';

      int pass_len = file.readBytesUntil('\n', pass, sizeof(pass) - 1);
      pass[pass_len] = '\0';

      // WiFi.softAP(ssid, pass);
      WiFi.begin(ssid, pass);
      Serial.printf("wifi loaded: %s / %s\n", ssid, pass);
    }
    file.close();
  } else {
    Serial.println("no wifis configured");
  }
}

/* ------------------------------------------------------------------------------- */
/* get sensor index from hexstring like "10F78DBA000800D7" */
// FIX: Prevent buffer overflow by declaring saddrstr inside loop
int getSensorIndex(const char *hexString) {
  char tmp[4];

  if (!hexString || scount <= 0) {
    return -1;
  }

  for (int i = 0; i < scount; i++) {
    char saddrstr[17];
    memset(saddrstr, '\0', sizeof(saddrstr));

    for (uint8_t j = 0; j < 8; j++) {
      sprintf(tmp, "%02X", sensor[i][j]);
      tmp[2] = 0;
      strcat(saddrstr, tmp);
    }
    if (strcmp(saddrstr, hexString) == 0) {
      return i;
    }
  }

  return -1;  // no sensor found
}

void loadSavedSensors() {
  // Reset names/calibration first so deleted mappings do not persist in RAM.
  memset(sensname, 0, sizeof(sensname));
  memset(senscal, 0, sizeof(senscal));

  if (SPIFFS.exists("/known_sensors.txt")) {
    file = SPIFFS.open("/known_sensors.txt");
    if (!file) {
      Serial.println("Failed to open known_sensors.txt");
      return;
    }

    while (file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) {
        continue;
      }

      int tab1 = line.indexOf('\t');
      if (tab1 <= 0) {
        continue;
      }
      int tab2 = line.indexOf('\t', tab1 + 1);

      String saddr = line.substring(0, tab1);
      String sname = (tab2 >= 0) ? line.substring(tab1 + 1, tab2) : line.substring(tab1 + 1);
      String scal = (tab2 >= 0) ? line.substring(tab2 + 1) : "0";

      int idx = getSensorIndex(saddr.c_str());
      if (idx >= 0 && idx < MAX_SENSORS) {
        strncpy(sensname[idx], sname.c_str(), sizeof(sensname[idx]) - 1);
        sensname[idx][sizeof(sensname[idx]) - 1] = '\0';
        senscal[idx] = scal.toFloat();
      }
    }

    file.close();
  }
}

/* ------------------------------------------------------------------------------- */
void loadMQTT() {
  if (SPIFFS.exists("/mqtt.txt")) {
    char tmpstr[8];
    memset(tmpstr, 0, sizeof(tmpstr));
    memset(mqtt_host, 0, sizeof(mqtt_host));
    memset(mqtt_user, 0, sizeof(mqtt_user));
    memset(mqtt_pass, 0, sizeof(mqtt_pass));
    memset(topicbase, 0, sizeof(topicbase));
    memset(myhostname, 0, sizeof(myhostname));

    File file = SPIFFS.open("/mqtt.txt", "r");
    if (!file) {
      Serial.println("Failed to open mqtt.txt");
      return;
    }

    while (file.available()) {
      // FIX: Add null-termination after readBytesUntil
      int host_len = file.readBytesUntil(':', mqtt_host, sizeof(mqtt_host) - 1);
      mqtt_host[host_len] = '\0';

      int port_len = file.readBytesUntil('\n', tmpstr, sizeof(tmpstr) - 1);
      tmpstr[port_len] = '\0';
      mqtt_port = atoi(tmpstr);
      if (mqtt_port < 1 || mqtt_port > 65535) mqtt_port = 1883;  // default

      int user_len = file.readBytesUntil(':', mqtt_user, sizeof(mqtt_user) - 1);
      mqtt_user[user_len] = '\0';

      int pass_len = file.readBytesUntil('\n', mqtt_pass, sizeof(mqtt_pass) - 1);
      mqtt_pass[pass_len] = '\0';

      int topic_len = file.readBytesUntil('\n', topicbase, sizeof(topicbase) - 1);
      topicbase[topic_len] = '\0';

      int host_len2 = file.readBytesUntil('\n', myhostname, sizeof(myhostname) - 1);
      myhostname[host_len2] = '\0';

      memset(tmpstr, 0, sizeof(tmpstr));
      int interval_len = file.readBytesUntil('\n', tmpstr, sizeof(tmpstr) - 1);
      tmpstr[interval_len] = '\0';
      interval = atoi(tmpstr);
    }
    file.close();
    Serial.printf("MQTT broker: %s:%d\nTopic prefix: %s\n", mqtt_host, mqtt_port, topicbase);
    Serial.printf("My hostname: %s\nInterval %d minutes\n", myhostname, interval);
  }
}

/* ------------------------------------------------------------------------------- */
void setup() {
  pinMode(APREQUEST, INPUT_PULLUP);
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LED_OFF);

  Serial.begin(115200);
  Serial.println("\n\nESP32 ds1820 to MQTT\n\n");

  // FIX: Initialize sensname array
  memset(sensname, 0, sizeof(sensname));
  memset(senscal, 0, sizeof(senscal));

  // Append last 3 octets of MAC to the default hostname
  uint8_t mymac[6];
  // wifi_get_macaddr(STATION_IF, mymac);
  WiFi.macAddress(mymac);
  char mac_end[8];
  sprintf(mac_end, "%02x%02x%02x", mymac[3], mymac[4], mymac[5]);
  strcat(myhostname, mac_end);
  Serial.printf("Default hostname: %s\n", myhostname);

  sensors.begin();
  delay(100);

  scount = sensors.getDeviceCount();
  sensors.setResolution(12);

  if (scount > MAX_SENSORS) {
    scount = MAX_SENSORS;
  }
  for (int i = 0; i < scount; i++) {
    if (sensors.getAddress(sensor[i], i)) {
      Serial.printf("Found sensor %d: ", i);
      for (uint8_t j = 0; j < 8; j++) {
        Serial.printf("%02X", sensor[i][j]);
      }
      Serial.printf(" resolution: %d bits\n", sensors.getResolution(sensor[i]));
    }
  }

  if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)) {
    Serial.println("SPIFFS mount failed, starting portal");
    startPortal();
    return;
  }

  // If there are no configuration files, start portal.
  if (SPIFFS.exists("/mqtt.txt") && SPIFFS.exists("/known_wifis.txt") && SPIFFS.exists("/known_sensors.txt")) {
    loadMQTT();
    loadWifis();

    loadSavedSensors();

    WiFi.mode(WIFI_STA);

    if (strlen(myhostname) > 0) WiFi.hostname(myhostname);
    mqttclient.setServer(mqtt_host, mqtt_port);
  } else {
    startPortal();
  }

#if CONFIG_FREERTOS_UNICORE
  const BaseType_t sensorCore = 0;
#else
  const BaseType_t sensorCore = (xPortGetCoreID() == 0) ? 1 : 0;
#endif
  xTaskCreatePinnedToCore(
    sensorMqttTask,
    "sensor_mqtt",
    8192,
    nullptr,
    1,
    &sensorTaskHandle,
    sensorCore);
}

/* ------------------------------------------------------------------------------- */
void loop() {
  if (portal_timer > 0) {  // portal mode
    server.handleClient();

    // blink onboard leds if we are in portal mode
    if (int(millis() % 1000) < 500) {
      digitalWrite(LED, LED_ON);
    } else {
      digitalWrite(LED, LED_OFF);
    }
  }
  if (digitalRead(APREQUEST) == LOW && portal_timer == 0) {
    startPortal();
  }
  if (millis() - portal_timer > APTIMEOUT && portal_timer > 0) {
    Serial.println("Portal timeout. Booting.");
    delay(1000);
    ESP.restart();
  }
  delay(10);
}

void sensorMqttTask(void *parameter) {
  (void)parameter;

  for (;;) {
    calibrationTick();

    if (portal_timer == 0) {
      digitalWrite(LED, LED_OFF);
      sread = 0;

      // If it has been more than interval minutes since last temperature read, do it now
      if ((millis() - mytime > 60000 * interval && interval > 0) || interval == 0) {
        if (onewire_wait == 0) {
          mytime = millis();
          sensors.setWaitForConversion(false);
          sensors.requestTemperatures();
          pinMode(ONE_WIRE_BUS, OUTPUT);
          digitalWrite(ONE_WIRE_BUS, HIGH);
          onewire_wait = 1;
        }
      }

      // after 1000ms per sensor the sensors should be read already
      if (scount > 0 && millis() - mytime > 1000 * scount && onewire_wait == 1) {
        mytime = millis();
        onewire_wait = 0;
        for (int i = 0; i < scount; i++) {
          if (sensors.isConnected(sensor[i])) {
            sens[i] = sensors.getTempC(sensor[i]);
            Serial.printf("sensor %d raw: 0x%X = %.4f°C\n", i, sensors.getTemp(sensor[i]), sens[i]);
          } else {
            sens[i] = NAN;
          }
        }
        sread = 1;
      }

      if (sread == 1) {
        // send MQTT
        WiFi.mode(WIFI_STA);

        sread = 0;
        if (WiFi.status() == WL_CONNECTED) {
          char json[32];  // This is enough space here
          char topic[320];
          // We send in deciCelsius
          // See: https://github.com/oh2mp/esp32_ble2mqtt/blob/main/DATAFORMATS.md
          //      TAG_DS1820 = 6
          if (mqttclient.connect(myhostname, mqtt_user, mqtt_pass)) {
            for (int i = 0; i < scount; i++) {
              // FIX: Use isnan() instead of direct NAN comparison
              if (!isnan(sens[i]) && sens[i] != 85.0 && sens[i] > -60.0 && strlen(sensname[i]) > 0) {
                if (calibrationActive && calibrationSelected[i]) {
                  continue;
                }
                float corrected = sens[i] + senscal[i];
                // why does round() not work?
                if (corrected >= 0) {
                  sprintf(json, "{\"type\":6,\"t\":%d}", int(corrected * 10.0 + 0.5));
                } else {
                  sprintf(json, "{\"type\":6,\"t\":%d}", int(corrected * 10.0 - 0.5));
                }
                if (strlen(topicbase) > 0) {
                  snprintf(topic, sizeof(topic), "%s/%s", topicbase, sensname[i]);
                } else {
                  snprintf(topic, sizeof(topic), "%s", sensname[i]);
                }
                mqttclient.publish(topic, json);
                Serial.printf("%s %s (raw %.4f°C, cal %.4f°C)\n", topic, json, sens[i], senscal[i]);
              }
            }
            mqttclient.disconnect();
          } else {
            Serial.printf("Failed to connect MQTT broker, state=%d\n", mqttclient.state());
          }

        } else {
          Serial.printf("Failed to connect WiFi, status=%d\n", WiFi.status());
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

bool saveCalibrationRowsToFile() {
  file = SPIFFS.open("/known_sensors.txt", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open known_sensors.txt for writing");
    return false;
  }

  for (int i = 0; i < calibrationRowCount; i++) {
    if (calibrationRowName[i][0] == '\0' || calibrationRowAddr[i][0] == '\0') {
      continue;
    }
    file.printf("%s\t%s\t%.6f\n", calibrationRowAddr[i], calibrationRowName[i], calibrationRowCal[i]);
  }

  file.close();
  loadSavedSensors();
  return true;
}

void finalizeCalibration() {
  for (int row = 0; row < calibrationRowCount; row++) {
    int idx = calibrationRowSensorIdx[row];
    if (idx >= 0 && idx < MAX_SENSORS && calibrationDone[idx]) {
      calibrationRowCal[row] = calibrationComputedOffset[idx];
    }
  }

  if (!saveCalibrationRowsToFile()) {
    Serial.println("Calibration finalize failed to save known_sensors.txt");
    calibrationActive = false;
    calibrationCompleted = false;
    return;
  }
  calibrationActive = false;
  calibrationCompleted = true;
  sensors.setWaitForConversion(false);
}

void calibrationTick() {
  if (!calibrationActive) {
    return;
  }

  unsigned long now = millis();
  if (now - calibrationStartMs >= CALIBRATION_TIMEOUT_MS) {
    calibrationTimedOut = true;
    finalizeCalibration();
    return;
  }

  if (now - calibrationLastSampleMs < CALIBRATION_SAMPLE_INTERVAL_MS) {
    return;
  }
  calibrationLastSampleMs = now;

  sensors.setWaitForConversion(true);
  sensors.requestTemperatures();

  for (int i = 0; i < scount; i++) {
    if (!calibrationSelected[i] || calibrationDone[i]) {
      continue;
    }

    float sample = sensors.getTempC(sensor[i]);
    calibrationLastTemp[i] = sample;
    if (isnan(sample) || sample == 85.0 || sample < -60.0 || sample > 60.0) {
      continue;
    }

    if (!calibrationReady[i]) {
      if (fabs(sample) <= CALIBRATION_READY_RANGE_C) {
        calibrationReady[i] = true;
        calibrationHasPrev[i] = true;
        calibrationPrevTemp[i] = sample;
      }
      continue;
    }

    if (!calibrationHasPrev[i]) {
      calibrationHasPrev[i] = true;
      calibrationPrevTemp[i] = sample;
      continue;
    }

    float delta = fabs(sample - calibrationPrevTemp[i]);
    calibrationPrevTemp[i] = sample;
    if (delta <= CALIBRATION_STABLE_DELTA_C) {
      calibrationSampleSum[i] += sample;
      calibrationSampleCount[i]++;
    }

    if (calibrationSampleCount[i] >= CALIBRATION_TARGET_SAMPLES) {
      calibrationDone[i] = true;
      calibrationDoneCount++;
      float average = calibrationSampleSum[i] / float(calibrationSampleCount[i]);
      calibrationComputedOffset[i] = -average;
    }
  }

  if (calibrationDoneCount >= calibrationSelectedCount) {
    finalizeCalibration();
  }
}

/* ------------------------------------------------------------------------------- */
// Portal code begins here
/* ------------------------------------------------------------------------------- */

void startPortal() {
  portal_timer = millis();
  WiFi.disconnect();
  delay(100);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("ESP32 DS1820 MQTT");

  server.on("/", HTTP_GET, httpRoot);
  server.on("/style.css", httpStyle);
  server.on("/wifis.html", httpWifis);
  server.on("/savewifi", httpSaveWifi);
  server.on("/sensors.html", httpSensors);
  server.on("/savesens", httpSaveSensors);
  server.on("/calsens", httpCalibrateSensors);
  server.on("/calstatus", httpCalibrationStatus);
  server.on("/mqtt.html", httpMQTT);
  server.on("/savemqtt", httpSaveMQTT);
  server.on("/boot", httpBoot);

  server.onNotFound([]() {
    server.sendHeader("Refresh", "1;url=/");
    server.send(404, "text/plain", "QSD QSY");
  });
  server.begin();
  Serial.println("Started portal");
}
/* ------------------------------------------------------------------------------- */

void httpRoot() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/index.html");
  if (!file) {
    Serial.println("Failed to open index.html");
    server.send(500, "text/plain", "Error: index.html not found");
    return;
  }

  html = file.readString();
  file.close();

  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpWifis() {
  String html;
  String tablerows = "";  // FIX: Use String instead of large char array
  char rowbuf[256];
  char ssid[33];
  char pass[64];
  int counter = 0;

  portal_timer = millis();

  file = SPIFFS.open("/wifis.html");
  if (!file) {
    Serial.println("Failed to open wifis.html");
    server.send(500, "text/plain", "Error: wifis.html not found");
    return;
  }

  html = file.readString();
  file.close();

  if (SPIFFS.exists("/known_wifis.txt")) {
    file = SPIFFS.open("/known_wifis.txt");
    if (!file) {
      Serial.println("Failed to open known_wifis.txt");
      server.send(500, "text/plain", "Error: cannot read wifis");
      return;
    }

    while (file.available()) {
      memset(rowbuf, '\0', sizeof(rowbuf));
      memset(ssid, '\0', sizeof(ssid));
      memset(pass, '\0', sizeof(pass));

      // FIX: Add null-termination
      int ssid_len = file.readBytesUntil('\t', ssid, sizeof(ssid) - 1);
      ssid[ssid_len] = '\0';

      int pass_len = file.readBytesUntil('\n', pass, sizeof(pass) - 1);
      pass[pass_len] = '\0';

      sprintf(rowbuf, "<tr><td>SSID</td><td><input type=\"text\" name=\"ssid%d\" maxlength=\"32\" value=\"%s\"></td></tr>", counter, ssid);
      tablerows += rowbuf;

      sprintf(rowbuf, "<tr><td>PASS</td><td><input type=\"text\" name=\"pass%d\" maxlength=\"63\" value=\"%s\"></td></tr>", counter, pass);
      tablerows += rowbuf;
      counter++;
    }
    file.close();
  }

  html.replace("###TABLEROWS###", tablerows);
  html.replace("###COUNTER###", String(counter));

  if (counter > 3) {
    html.replace("table-row", "none");
  }

  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpSaveWifi() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/known_wifis.txt", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open known_wifis.txt for writing");
    server.send(500, "text/plain", "Error: cannot save wifis");
    return;
  }

  for (int i = 0; i < server.arg("counter").toInt(); i++) {
    if (server.arg("ssid" + String(i)).length() > 0) {
      file.print(server.arg("ssid" + String(i)));
      file.print("\t");
      file.print(server.arg("pass" + String(i)));
      file.print("\n");
    }
  }
  // Add new
  if (server.arg("ssid").length() > 0) {
    file.print(server.arg("ssid"));
    file.print("\t");
    file.print(server.arg("pass"));
    file.print("\n");
  }
  file.close();

  file = SPIFFS.open("/ok.html");
  if (!file) {
    Serial.println("Failed to open ok.html");
    server.send(500, "text/plain", "Error: ok.html not found");
    return;
  }

  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "3;url=/");
  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpSensors() {
  String html;
  String tablerows = "";  // FIX: Use String instead of large char array
  char rowbuf[512];
  char calbuf[24];
  char sname[25];
  char saddrstr[17];
  char tmp[4];
  int counter = 0;
  uint8_t unexistents = 0;

  portal_timer = millis();

  file = SPIFFS.open("/sensors.html");
  if (!file) {
    Serial.println("Failed to open sensors.html");
    server.send(500, "text/plain", "Error: sensors.html not found");
    return;
  }

  html = file.readString();
  file.close();

  for (int i = 0; i < scount; i++) {
    memset(saddrstr, '\0', sizeof(saddrstr));
    memset(sname, '\0', sizeof(sname));
    memset(tmp, '\0', sizeof(tmp));

    for (uint8_t j = 0; j < 8; j++) {
      sprintf(tmp, "%02X", sensor[i][j]);
      strcat(saddrstr, tmp);
      tmp[2] = 0;
      sprintf(tmp, "%02X:", sensor[i][j]);
      strcat(sname, tmp);
      tmp[3] = 0;
    }
    if (saddrstr[0] != 0) {
      sname[strlen(sname) - 1] = 0;
      snprintf(calbuf, sizeof(calbuf), "%.4f", senscal[i]);
      snprintf(rowbuf, sizeof(rowbuf), "<tr><td>%s</td><td><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\"></td><td><input type=\"text\" name=\"scal%d\" maxlength=\"20\" value=\"%s\"></td><td><input type=\"checkbox\" name=\"calrun%d\" value=\"1\"></td></tr>", sname, i, sensname[i], i, calbuf, i);
      tablerows += rowbuf;
      snprintf(rowbuf, sizeof(rowbuf), "<input type=\"hidden\" name=\"saddr%d\" value=\"%s\">", i, saddrstr);
      tablerows += rowbuf;
      counter++;
    }
  }

  if (SPIFFS.exists("/known_sensors.txt")) {
    file = SPIFFS.open("/known_sensors.txt");
    if (!file) {
      Serial.println("Failed to open known_sensors.txt");
      server.send(500, "text/plain", "Error: cannot read sensors");
      return;
    }

    while (file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) {
        continue;
      }

      int tab1 = line.indexOf('\t');
      if (tab1 <= 0) {
        continue;
      }
      int tab2 = line.indexOf('\t', tab1 + 1);

      String addr = line.substring(0, tab1);
      String name = (tab2 >= 0) ? line.substring(tab1 + 1, tab2) : line.substring(tab1 + 1);
      String cal = (tab2 >= 0) ? line.substring(tab2 + 1) : "0";

      if (getSensorIndex(addr.c_str()) == -1) {
        if (addr.length() > 0) {
          if (unexistents == 0) {
            unexistents = 1;
            tablerows += "<tr><td colspan=\"4\"><hr /><b>Unexistent but saved sensors</b></td></tr>";
          }
          snprintf(rowbuf, sizeof(rowbuf), "<tr><td>%s</td><td><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\"></td><td><input type=\"text\" name=\"scal%d\" maxlength=\"20\" value=\"%s\"></td><td><input type=\"checkbox\" name=\"calrun%d\" value=\"1\" disabled></td></tr>", addr.c_str(), counter, name.c_str(), counter, cal.c_str(), counter);
          tablerows += rowbuf;
          snprintf(rowbuf, sizeof(rowbuf), "<input type=\"hidden\" name=\"saddr%d\" value=\"%s\">", counter, addr.c_str());
          tablerows += rowbuf;
          counter++;
        }
      }
    }
    file.close();
  }

  html.replace("###TABLEROWS###", tablerows);
  html.replace("###COUNTER###", String(counter));
  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpSaveSensors() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/known_sensors.txt", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open known_sensors.txt for writing");
    server.send(500, "text/plain", "Error: cannot save sensors");
    return;
  }

  for (int i = 0; i < server.arg("counter").toInt(); i++) {
    if (server.arg("sname" + String(i)).length() > 0) {
      float cal = server.arg("scal" + String(i)).toFloat();
      file.printf("%s\t%s\t%.6f\n",
                  server.arg("saddr" + String(i)).c_str(),
                  server.arg("sname" + String(i)).c_str(),
                  cal);
    }
  }
  file.close();
  loadSavedSensors();  // reread

  file = SPIFFS.open("/ok.html");
  if (!file) {
    Serial.println("Failed to open ok.html");
    server.send(500, "text/plain", "Error: ok.html not found");
    return;
  }

  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "3;url=/");
  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpCalibrateSensors() {
  portal_timer = millis();

  if (calibrationActive) {
    server.send(409, "text/plain", "Calibration is already running.");
    return;
  }

  calibrationRowCount = 0;
  calibrationSelectedCount = 0;
  calibrationDoneCount = 0;
  calibrationCompleted = false;
  calibrationTimedOut = false;
  calibrationStartMs = millis();
  calibrationLastSampleMs = 0;

  memset(calibrationSelected, 0, sizeof(calibrationSelected));
  memset(calibrationReady, 0, sizeof(calibrationReady));
  memset(calibrationDone, 0, sizeof(calibrationDone));
  memset(calibrationHasPrev, 0, sizeof(calibrationHasPrev));
  memset(calibrationSampleSum, 0, sizeof(calibrationSampleSum));
  memset(calibrationSampleCount, 0, sizeof(calibrationSampleCount));
  memset(calibrationComputedOffset, 0, sizeof(calibrationComputedOffset));
  for (int i = 0; i < MAX_SENSORS; i++) {
    calibrationLastTemp[i] = NAN;
    calibrationPrevTemp[i] = 0;
  }

  int counter = server.arg("counter").toInt();
  if (counter > MAX_SENSOR_ROWS) {
    counter = MAX_SENSOR_ROWS;
  }

  for (int i = 0; i < counter; i++) {
    String nameArg = server.arg("sname" + String(i));
    if (nameArg.length() == 0) {
      continue;
    }

    String addrArg = server.arg("saddr" + String(i));
    if (addrArg.length() == 0 || calibrationRowCount >= MAX_SENSOR_ROWS) {
      continue;
    }

    int row = calibrationRowCount;
    calibrationRowCount++;

    strncpy(calibrationRowAddr[row], addrArg.c_str(), sizeof(calibrationRowAddr[row]) - 1);
    calibrationRowAddr[row][sizeof(calibrationRowAddr[row]) - 1] = '\0';
    strncpy(calibrationRowName[row], nameArg.c_str(), sizeof(calibrationRowName[row]) - 1);
    calibrationRowName[row][sizeof(calibrationRowName[row]) - 1] = '\0';
    calibrationRowCal[row] = server.arg("scal" + String(i)).toFloat();

    int idx = getSensorIndex(addrArg.c_str());
    calibrationRowSensorIdx[row] = idx;

    bool selectedForCalibration = (server.arg("calrun" + String(i)).length() > 0);
    if (selectedForCalibration && idx >= 0 && idx < MAX_SENSORS && !calibrationSelected[idx]) {
      calibrationSelected[idx] = true;
      calibrationSelectedCount++;
    }
  }

  if (calibrationSelectedCount == 0) {
    server.send(400, "text/plain", "Select at least one existing sensor for calibration.");
    return;
  }

  // Persist names/manual offsets from the form first so calibration updates the current UI state.
  if (!saveCalibrationRowsToFile()) {
    server.send(500, "text/plain", "Error: cannot save sensors");
    return;
  }
  calibrationActive = true;

  String html = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"></head><body>";
  html += "<table>";
  html += "<tr><td><b>Calibration Running</b></td></tr>";
  html += "<tr><td>Instructions:</td></tr>";
  html += "<tr><td>1. Put selected sensors in well-mixed ice water.</td></tr>";
  html += "<tr><td>2. Wait until each selected sensor reaches within +/-2.0 C from zero.</td></tr>";
  html += "<tr><td>3. Sampling starts automatically after that and collects 10 stabilized values per sensor.</td></tr>";
  html += "<tr><td>4. Timeout is 5 minutes. Completed sensors are still saved if timeout happens.</td></tr>";
  html += "<tr><td><pre id=\"calstatus\">Starting...</pre></td></tr>";
  html += "<tr><td><a class=\"fakebutton\" href=\"/sensors.html\">back to sensors</a></td></tr>";
  html += "</table>";
  html += "<script>";
  html += "function u(){fetch('/calstatus').then(r=>r.json()).then(j=>{";
  html += "let t='Elapsed: '+j.elapsed_s+'s / '+j.timeout_s+'s\\n';";
  html += "t+='Completed: '+j.done_sensors+'/'+j.selected_sensors+' sensors\\n';";
  html += "if(j.timed_out){t+='Status: timed out\\n';} else if(j.active){t+='Status: running\\n';} else if(j.completed){t+='Status: completed\\n';}";
  html += "for(let i=0;i<j.sensors.length;i++){let s=j.sensors[i];t+='- '+s.name+': '+s.state+', samples '+s.samples+'/'+s.target; if(s.last_valid){t+=', last '+s.last.toFixed(3)+' C';} if(s.done){t+=', offset '+s.offset.toFixed(4)+' C';} t+='\\n';}";
  html += "document.getElementById('calstatus').textContent=t; if(j.active){setTimeout(u,1000);} });}";
  html += "u();";
  html += "</script></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

void httpCalibrationStatus() {
  portal_timer = millis();

  unsigned long now = millis();
  unsigned long elapsed = calibrationStartMs > 0 ? (now - calibrationStartMs) : 0;
  if (elapsed > CALIBRATION_TIMEOUT_MS) {
    elapsed = CALIBRATION_TIMEOUT_MS;
  }

  String json = "{";
  json += "\"active\":" + String(calibrationActive ? 1 : 0) + ",";
  json += "\"completed\":" + String(calibrationCompleted ? 1 : 0) + ",";
  json += "\"timed_out\":" + String(calibrationTimedOut ? 1 : 0) + ",";
  json += "\"elapsed_s\":" + String(elapsed / 1000) + ",";
  json += "\"timeout_s\":" + String(CALIBRATION_TIMEOUT_MS / 1000) + ",";
  json += "\"selected_sensors\":" + String(calibrationSelectedCount) + ",";
  json += "\"done_sensors\":" + String(calibrationDoneCount) + ",";
  json += "\"sensors\":[";

  bool first = true;
  for (int i = 0; i < scount; i++) {
    if (!calibrationSelected[i]) {
      continue;
    }
    if (!first) {
      json += ",";
    }
    first = false;

    String state = "waiting";
    if (calibrationDone[i]) {
      state = "done";
    } else if (calibrationReady[i]) {
      state = "sampling";
    }

    bool lastValid = !isnan(calibrationLastTemp[i]);
    json += "{";
    json += "\"name\":\"" + String(sensname[i]) + "\",";
    json += "\"state\":\"" + state + "\",";
    json += "\"samples\":" + String(calibrationSampleCount[i]) + ",";
    json += "\"target\":" + String(CALIBRATION_TARGET_SAMPLES) + ",";
    json += "\"done\":" + String(calibrationDone[i] ? 1 : 0) + ",";
    json += "\"last_valid\":" + String(lastValid ? 1 : 0) + ",";
    if (lastValid) {
      json += "\"last\":" + String(calibrationLastTemp[i], 4) + ",";
    } else {
      json += "\"last\":0,";
    }
    json += "\"offset\":" + String(calibrationComputedOffset[i], 6);
    json += "}";
  }

  json += "]}";
  server.send(200, "application/json", json);
}

/* ------------------------------------------------------------------------------- */

void httpMQTT() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/mqtt.html");
  if (!file) {
    Serial.println("Failed to open mqtt.html");
    server.send(500, "text/plain", "Error: mqtt.html not found");
    return;
  }

  html = file.readString();
  file.close();

  html.replace("###HOSTPORT###", String(mqtt_host) + ":" + String(mqtt_port));
  html.replace("###USERPASS###", String(mqtt_user) + ":" + String(mqtt_pass));
  html.replace("###TOPICBASE###", String(topicbase));
  html.replace("###MYHOSTNAME###", String(myhostname));
  html.replace("###INTERVAL###", String(interval));

  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */
void httpSaveMQTT() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/mqtt.txt", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open mqtt.txt for writing");
    server.send(500, "text/plain", "Error: cannot save MQTT config");
    return;
  }

  file.printf("%s\n", server.arg("hostport").c_str());
  file.printf("%s\n", server.arg("userpass").c_str());
  file.printf("%s\n", server.arg("topicbase").c_str());
  file.printf("%s\n", server.arg("myhostname").c_str());
  file.printf("%s\n", server.arg("interval").c_str());
  file.close();

  loadMQTT();  // reread

  file = SPIFFS.open("/ok.html");
  if (!file) {
    Serial.println("Failed to open ok.html");
    server.send(500, "text/plain", "Error: ok.html not found");
    return;
  }

  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "2;url=/");
  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpStyle() {
  portal_timer = millis();
  String css;

  File file = SPIFFS.open("/style.css");
  if (!file) {
    Serial.println("Failed to open style.css");
    server.send(500, "text/css", "/* Error: style.css not found */");
    return;
  }

  css = file.readString();
  file.close();
  server.send(200, "text/css", css);
}

/* ------------------------------------------------------------------------------- */

void httpBoot() {
  portal_timer = millis();
  String html;

  File file = SPIFFS.open("/ok.html");
  if (!file) {
    Serial.println("Failed to open ok.html");
    server.send(500, "text/html; charset=UTF-8", "<html><body>Error: ok.html not found</body></html>");
    return;
  }

  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "2;url=about:blank");
  server.send(200, "text/html; charset=UTF-8", html);
  delay(1000);
  ESP.restart();
}

/* ------------------------------------------------------------------------------- */
