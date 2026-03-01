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


float sens[MAX_SENSORS];         // sensor values
char sensname[MAX_SENSORS][24];  // sensor names
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
  char sname[25];
  char saddrstr[17];

  // Reset names first so deleted mappings do not persist in RAM.
  memset(sensname, 0, sizeof(sensname));

  if (SPIFFS.exists("/known_sensors.txt")) {
    file = SPIFFS.open("/known_sensors.txt");
    if (!file) {
      Serial.println("Failed to open known_sensors.txt");
      return;
    }

    while (file.available()) {
      memset(sname, '\0', sizeof(sname));
      memset(saddrstr, '\0', sizeof(saddrstr));

      // FIX: Add null-termination after readBytesUntil
      int addr_len = file.readBytesUntil('\t', saddrstr, sizeof(saddrstr) - 1);
      saddrstr[addr_len] = '\0';

      int name_len = file.readBytesUntil('\n', sname, sizeof(sname) - 1);
      sname[name_len] = '\0';

      // FIX: Store result of getSensorIndex once and check bounds
      int idx = getSensorIndex(saddrstr);
      if (idx >= 0 && idx < MAX_SENSORS) {
        strncpy(sensname[idx], sname, sizeof(sensname[idx]) - 1);
        sensname[idx][sizeof(sensname[idx]) - 1] = '\0';
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
                // why does round() not work?
                if (sens[i] >= 0) {
                  sprintf(json, "{\"type\":6,\"t\":%d}", int(sens[i] * 10.0 + 0.5));
                } else {
                  sprintf(json, "{\"type\":6,\"t\":%d}", int(sens[i] * 10.0 - 0.5));
                }
                if (strlen(topicbase) > 0) {
                  snprintf(topic, sizeof(topic), "%s/%s", topicbase, sensname[i]);
                } else {
                  snprintf(topic, sizeof(topic), "%s", sensname[i]);
                }
                mqttclient.publish(topic, json);
                Serial.printf("%s %s\n", topic, json);
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
  char rowbuf[256];
  char sname[25];
  char saddrstr[17];
  char tmp[4];
  int counter = 0;
  uint8_t unexistents = 0;
  DeviceAddress saddr;

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
      sprintf(rowbuf, "<tr><td>%s<br /><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">", sname, i, sensname[i]);
      tablerows += rowbuf;
      sprintf(rowbuf, "<input type=\"hidden\" name=\"saddr%d\" value=\"%s\"></td></tr>", i, saddrstr);
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
      memset(sname, '\0', sizeof(sname));
      memset(saddrstr, '\0', sizeof(saddrstr));

      // FIX: Add null-termination
      int addr_len = file.readBytesUntil('\t', saddrstr, sizeof(saddrstr) - 1);
      saddrstr[addr_len] = '\0';

      int name_len = file.readBytesUntil('\n', sname, sizeof(sname) - 1);
      sname[name_len] = '\0';

      if (getSensorIndex(saddrstr) == -1) {
        if (saddrstr[0] != 0) {
          if (unexistents == 0) {
            unexistents = 1;
            tablerows += "<tr><td><hr /><b>Unexistent but saved sensors</b></td></tr>";
          }
          // FIX: Check length before accessing last character
          size_t sname_len = strlen(sname);
          if (sname_len > 0 && sname[sname_len - 1] == 13) {
            sname[sname_len - 1] = 0;
          }
          sprintf(rowbuf, "<tr><td>%s<br /><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">", saddrstr, counter, sname);
          tablerows += rowbuf;
          sprintf(rowbuf, "<input type=\"hidden\" name=\"saddr%d\" value=\"%s\"></td></tr>", counter, saddrstr);
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
      file.print(server.arg("saddr" + String(i)));
      file.print("\t");
      file.print(server.arg("sname" + String(i)));
      file.print("\n");
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
