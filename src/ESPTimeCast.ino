#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <MD_Parola.h>
#include <MD_MAX72XX.h>
#include <SPI.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <sntp.h>
#include <time.h>

#include "mfactoryfont.h"  // Replace with your font, or comment/remove if not using custom
#include "tz_lookup.h" // Timezone lookup, do not duplicate mapping here!

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define CLK_PIN   12
#define DATA_PIN  15
#define CS_PIN    13
MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

AsyncWebServer server(80);

char ssid[32] = "";
char password[32] = "";
char openWeatherApiKey[64] = "";
char openWeatherCity[64] = "";
char openWeatherCountry[64] = "";
char weatherUnits[12] = "metric";
char timeZone[64] = "";
unsigned long clockDuration = 10000;
unsigned long weatherDuration = 5000;

// ADVANCED SETTINGS
int brightness = 7;
bool flipDisplay = false;
bool twelveHourToggle = false;
bool showDayOfWeek = true;
bool showHumidity = false;
char ntpServer1[64] = "pool.ntp.org";
char ntpServer2[64] = "time.nist.gov";

WiFiClient client;
const byte DNS_PORT = 53;
DNSServer dnsServer;

String currentTemp = "";
String weatherDescription = "";
bool weatherAvailable = false;
bool weatherFetched = false;
bool weatherFetchInitiated = false;
bool isAPMode = false;
char tempSymbol = 'C';

unsigned long lastSwitch = 0;
unsigned long lastColonBlink = 0;
int displayMode = 0;
int currentHumidity = -1;

bool ntpSyncSuccessful = false;

char daysOfTheWeek[7][12] = {"&", "*", "/", "?", "@", "=", "$"};

// NTP Synchronization State Machine
enum NtpState {
  NTP_IDLE,
  NTP_SYNCING,
  NTP_SUCCESS,
  NTP_FAILED
};

NtpState ntpState = NTP_IDLE;
unsigned long ntpStartTime = 0;
const int ntpTimeout = 30000; // 30 seconds
const int maxNtpRetries = 30;
int ntpRetryCount = 0;

void printConfigToSerial() {
  Serial.println(F("========= Loaded Configuration ========="));
  Serial.print(F("WiFi SSID: ")); Serial.println(ssid);
  Serial.print(F("WiFi Password: ")); Serial.println(password);
  Serial.print(F("OpenWeather City: ")); Serial.println(openWeatherCity);
  Serial.print(F("OpenWeather Country: ")); Serial.println(openWeatherCountry);
  Serial.print(F("OpenWeather API Key: ")); Serial.println(openWeatherApiKey);
  Serial.print(F("Temperature Unit: ")); Serial.println(weatherUnits);
  Serial.print(F("Clock duration: ")); Serial.println(clockDuration);
  Serial.print(F("Weather duration: ")); Serial.println(weatherDuration);
  Serial.print(F("TimeZone (IANA): ")); Serial.println(timeZone);
  Serial.print(F("Brightness: ")); Serial.println(brightness);
  Serial.print(F("Flip Display: ")); Serial.println(flipDisplay ? "Yes" : "No");
  Serial.print(F("Show 12h Clock: ")); Serial.println(twelveHourToggle ? "Yes" : "No");
  Serial.print(F("Show Day of the Week: ")); Serial.println(showDayOfWeek ? "Yes" : "No");
  Serial.print(F("Show Humidity ")); Serial.println(showHumidity ? "Yes" : "No");
  Serial.print(F("NTP Server 1: ")); Serial.println(ntpServer1);
  Serial.print(F("NTP Server 2: ")); Serial.println(ntpServer2);
  Serial.println(F("========================================"));
  Serial.println();
}

void loadConfig() {
  Serial.println(F("[CONFIG] Loading configuration..."));
  if (!LittleFS.begin()) {
    Serial.println(F("[ERROR] LittleFS mount failed"));
    return;
  }

  if (!LittleFS.exists("/config.json")) {
    Serial.println(F("[CONFIG] config.json not found, creating with defaults..."));
    DynamicJsonDocument doc(512);
    doc[F("ssid")] = "";
    doc[F("password")] = "";
    doc[F("openWeatherApiKey")] = "";
    doc[F("openWeatherCity")] = "";
    doc[F("openWeatherCountry")] = "";
    doc[F("weatherUnits")] = "metric";
    doc[F("clockDuration")] = 10000;
    doc[F("weatherDuration")] = 5000;
    doc[F("timeZone")] = "";
    doc[F("brightness")] = brightness;
    doc[F("flipDisplay")] = flipDisplay;
    doc[F("twelveHourToggle")] = twelveHourToggle;
    doc[F("showDayOfWeek")] = showDayOfWeek;
    doc[F("showHumidity")] = showHumidity;
    doc[F("ntpServer1")] = ntpServer1;
    doc[F("ntpServer2")] = ntpServer2;
    File f = LittleFS.open("/config.json", "w");
    if (f) {
      serializeJsonPretty(doc, f);
      f.close();
      Serial.println(F("[CONFIG] Default config.json created."));
    } else {
      Serial.println(F("[ERROR] Failed to create default config.json"));
    }
  }
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("[ERROR] Failed to open config.json"));
    return;
  }
  size_t size = configFile.size();
  if (size == 0 || size > 1024) {
    Serial.println(F("[ERROR] Invalid config file size"));
    configFile.close();
    return;
  }
  String jsonString = configFile.readString();
  configFile.close();
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, jsonString);
  if (error) {
    Serial.print(F("[ERROR] JSON parse failed: "));
    Serial.println(error.f_str());
    return;
  }
  if (doc.containsKey("ssid")) strlcpy(ssid, doc["ssid"], sizeof(ssid));
  if (doc.containsKey("password")) strlcpy(password, doc["password"], sizeof(password));
  if (doc.containsKey("openWeatherApiKey")) strlcpy(openWeatherApiKey, doc["openWeatherApiKey"], sizeof(openWeatherApiKey));
  if (doc.containsKey("openWeatherCity")) strlcpy(openWeatherCity, doc["openWeatherCity"], sizeof(openWeatherCity));
  if (doc.containsKey("openWeatherCountry")) strlcpy(openWeatherCountry, doc["openWeatherCountry"], sizeof(openWeatherCountry));
  if (doc.containsKey("weatherUnits")) strlcpy(weatherUnits, doc["weatherUnits"], sizeof(weatherUnits));
  if (doc.containsKey("clockDuration")) clockDuration = doc["clockDuration"];
  if (doc.containsKey("weatherDuration")) weatherDuration = doc["weatherDuration"];
  if (doc.containsKey("timeZone")) strlcpy(timeZone, doc["timeZone"], sizeof(timeZone));
  if (doc.containsKey("brightness")) brightness = doc["brightness"];
  if (doc.containsKey("flipDisplay")) flipDisplay = doc["flipDisplay"];
  if (doc.containsKey("twelveHourToggle")) twelveHourToggle = doc["twelveHourToggle"];
  if (doc.containsKey("showDayOfWeek")) showDayOfWeek = doc["showDayOfWeek"]; // <-- NEW
  if (doc.containsKey("showHumidity")) showHumidity = doc["showHumidity"]; else showHumidity = false;
  if (doc.containsKey("ntpServer1")) strlcpy(ntpServer1, doc["ntpServer1"], sizeof(ntpServer1));
  if (doc.containsKey("ntpServer2")) strlcpy(ntpServer2, doc["ntpServer2"], sizeof(ntpServer2));
  if (strcmp(weatherUnits, "imperial") == 0)
    tempSymbol = 'F';
  else if (strcmp(weatherUnits, "standard") == 0)
    tempSymbol = 'K';
  else
    tempSymbol = 'C';
  Serial.println(F("[CONFIG] Configuration loaded."));
}

void connectWiFi() {
  Serial.println(F("[WIFI] Connecting to WiFi..."));
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();
  const unsigned long timeout = 15000;
  unsigned long animTimer = 0;
  int animFrame = 0;
  bool animating = true;
  while (animating) {
    unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("[WiFi] Connected: ") + WiFi.localIP().toString());
      isAPMode = false;
      animating = false;
      break;
    } else if (now - startAttemptTime >= timeout) {
      Serial.println(F("\r\n[WiFi] Failed. Starting AP mode..."));
      WiFi.softAP("ESPTimeCast", "12345678");
      Serial.print(F("AP IP address: "));
      Serial.println(WiFi.softAPIP());
      dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
      isAPMode = true;
      animating = false;
      Serial.println(F("[WIFI] AP Mode Started"));
      break;
    }
    if (now - animTimer > 750) {
      animTimer = now;
      P.setTextAlignment(PA_CENTER);
      switch (animFrame % 3) {
        case 0: P.print("WiFi©"); break;
        case 1: P.print("WiFiª"); break;
        case 2: P.print("WiFi«"); break;
      }
      animFrame++;
    }
    yield();
  }
}

void setupTime() {
  sntp_stop();
  Serial.println(F("[TIME] Starting NTP sync..."));
  configTime(0, 0, ntpServer1, ntpServer2); // Use custom NTP servers
  setenv("TZ", ianaToPosix(timeZone), 1);
  tzset();
  ntpState = NTP_SYNCING;
  ntpStartTime = millis();
  ntpRetryCount = 0;
  ntpSyncSuccessful = false; // Reset the flag
}

void setupWebServer() {
  Serial.println(F("[WEBSERVER] Setting up web server..."));
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /"));
    request->send(LittleFS, "/index.html", "text/html");
  });
  server.on("/config.json", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /config.json"));
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
      Serial.println(F("[WEBSERVER] Error opening /config.json"));
      request->send(500, "application/json", "{\"error\":\"Failed to open config.json\"}");
      return;
    }
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      Serial.print(F("[WEBSERVER] Error parsing /config.json: "));
      Serial.println(err.f_str());
      request->send(500, "application/json", "{\"error\":\"Failed to parse config.json\"}");
      return;
    }
    doc[F("mode")] = isAPMode ? "ap" : "sta";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /save"));    
    DynamicJsonDocument doc(2048);
    for (int i = 0; i < request->params(); i++) {
      const AsyncWebParameter* p = request->getParam(i);
      String n = p->name();
      String v = p->value();
      if (n == "brightness") doc[n] = v.toInt();
      else if (n == "flipDisplay") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "twelveHourToggle") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "showDayOfWeek") doc[n] = (v == "true" || v == "on" || v == "1"); 
      else if (n == "showHumidity") doc[n] = (v == "true" || v == "on" || v == "1");
      else doc[n] = v;
    }
    if (LittleFS.exists("/config.json")) {
      LittleFS.rename("/config.json", "/config.bak");
    }
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
      Serial.println(F("[WEBSERVER] Failed to open /config.json for writing"));
      DynamicJsonDocument errorDoc(256);
      errorDoc[F("error")] = "Failed to write config";
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }
    serializeJson(doc, f);
    f.close();
    File verify = LittleFS.open("/config.json", "r");
    DynamicJsonDocument test(2048);
    DeserializationError err = deserializeJson(test, verify);
    verify.close();
    if (err) {
      Serial.print(F("[WEBSERVER] Config corrupted after save: "));
      Serial.println(err.f_str());
      DynamicJsonDocument errorDoc(256);
      errorDoc[F("error")] = "Config corrupted. Reboot cancelled.";
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }
    DynamicJsonDocument okDoc(128);
    okDoc[F("message")] = "Saved successfully. Rebooting...";
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
    Serial.println(F("[WEBSERVER] Rebooting..."));
    request->onDisconnect([]() {
      Serial.println(F("[WEBSERVER] Rebooting..."));
      ESP.restart();
    });
  });

  server.on("/restore", HTTP_POST, [](AsyncWebServerRequest *request){
    Serial.println(F("[WEBSERVER] Request: /restore"));
    if (LittleFS.exists("/config.bak")) {
      File src = LittleFS.open("/config.bak", "r");
      if (!src) {
        Serial.println(F("[WEBSERVER] Failed to open /config.bak"));
        DynamicJsonDocument errorDoc(128);
        errorDoc[F("error")] = "Failed to open backup file.";
        String response;
        serializeJson(errorDoc, response);
        request->send(500, "application/json", response);
        return;
      }
      File dst = LittleFS.open("/config.json", "w");
      if (!dst) {
        src.close();
        Serial.println(F("[WEBSERVER] Failed to open /config.json for writing"));
        DynamicJsonDocument errorDoc(128);
        errorDoc[F("error")] = "Failed to open config for writing.";
        String response;
        serializeJson(errorDoc, response);
        request->send(500, "application/json", response);
        return;
      }
      // Copy contents
      while (src.available()) {
        dst.write(src.read());
      }
      src.close();
      dst.close();

      DynamicJsonDocument okDoc(128);
      okDoc[F("message")] = "✅ Backup restored! Device will now reboot.";
      String response;
      serializeJson(okDoc, response);
      request->send(200, "application/json", response);
      request->onDisconnect([]() {
        Serial.println(F("[WEBSERVER] Rebooting after restore..."));
        ESP.restart();
      });

    } else {
      Serial.println(F("[WEBSERVER] No backup found"));
      DynamicJsonDocument errorDoc(128);
      errorDoc[F("error")] = "No backup found.";
      String response;
      serializeJson(errorDoc, response);
      request->send(404, "application/json", response);
    }
  });

  server.on("/ap_status", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.print(F("[WEBSERVER] Request: /ap_status. isAPMode = "));
    Serial.println(isAPMode);
    String json = "{\"isAP\": ";
    json += (isAPMode) ? "true" : "false";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.begin();
  Serial.println(F("[WEBSERVER] Web server started"));
}

void fetchWeather() {
  Serial.println(F("[WEATHER] Fetching weather data..."));
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WEATHER] Skipped: WiFi not connected"));
    weatherAvailable = false;
    weatherFetched = false;
    return;
  }
  if (!openWeatherApiKey || strlen(openWeatherApiKey) != 32) {
    Serial.println(F("[WEATHER] Skipped: Invalid API key (must be exactly 32 characters)"));
    weatherAvailable = false;
    weatherFetched = false;
    return;
  }
  if (!(strlen(openWeatherCity) > 0 && strlen(openWeatherCountry) > 0)) {
    Serial.println(F("[WEATHER] Skipped: City or Country is empty."));
    return;
  }

  Serial.println(F("[WEATHER] Connecting to OpenWeatherMap..."));
  const char* host = "api.openweathermap.org";
  String url = "/data/2.5/weather?q=" + String(openWeatherCity) + "," + String(openWeatherCountry) + "&appid=" + openWeatherApiKey + "&units=" + String(weatherUnits);
  Serial.println(F("[WEATHER] URL: ") + url);

  IPAddress ip;
  if (!WiFi.hostByName(host, ip)) {
    Serial.println(F("[WEATHER] DNS lookup failed!"));
    weatherAvailable = false;
    return;
  }

  if (!client.connect(host, 80)) {
    Serial.println(F("[WEATHER] Connection failed"));
    weatherAvailable = false;
    return;
  }

  Serial.println(F("[WEATHER] Connected, sending request..."));
  String request = String("GET ") + url + " HTTP/1.1\r\n" +
                   "Host: " + host + "\r\n" +
                   "Connection: close\r\n\r\n";

  if (!client.print(request)) {
    Serial.println(F("[WEATHER] Failed to send request!"));
    client.stop();
    weatherAvailable = false;
    return;
  }

  unsigned long weatherStart = millis();
  const unsigned long weatherTimeout = 10000; // 10 seconds max

  bool isBody = false;
  String payload = "";
  String line = "";

  // Read headers and then the entire body at once
  while ((client.connected() || client.available()) &&
         millis() - weatherStart < weatherTimeout &&
         WiFi.status() == WL_CONNECTED) {
    line = client.readStringUntil('\n');
    if (line.length() == 0) continue;

    if (line.startsWith("HTTP/1.1")) {
      int statusCode = line.substring(9, 12).toInt();
      if (statusCode != 200) {
        Serial.print(F("[WEATHER] HTTP error: "));
        Serial.println(statusCode);
        client.stop();
        weatherAvailable = false;
        return;
      }
    }

    if (!isBody && line == "\r") {
      isBody = true;
      // Read the entire body at once
      while (client.available()) {
        payload += (char)client.read();
      }
      break; // All done!
    }
    yield();
    delay(1);
  }
  client.stop();

  if (millis() - weatherStart >= weatherTimeout) {
    Serial.println(F("[WEATHER] ERROR: Weather fetch timed out!"));
    weatherAvailable = false;
    return;
  }

  Serial.println(F("[WEATHER] Response received."));
  Serial.println(F("[WEATHER] Payload: ") + payload);

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print(F("[WEATHER] JSON parse error: "));
    Serial.println(error.f_str());
    weatherAvailable = false;
    return;
  }

  if (doc.containsKey("main") && doc["main"].containsKey("temp")) {
    float temp = doc["main"]["temp"];
    currentTemp = String((int)round(temp)) + "º";
    Serial.printf("[WEATHER] Temp: %s\n", currentTemp.c_str());
    weatherAvailable = true;
  } else {
    Serial.println(F("[WEATHER] Temperature not found in JSON payload"));
    weatherAvailable = false;
    return;
  }

  if (doc.containsKey("main") && doc["main"].containsKey("humidity")) {
  currentHumidity = doc["main"]["humidity"];
  Serial.printf("[WEATHER] Humidity: %d%%\n", currentHumidity);
  } else {
    currentHumidity = -1;
  }

  if (doc.containsKey("weather") && doc["weather"].is<JsonArray>() && doc["weather"][0].containsKey("main")) {
    const char* desc = doc["weather"][0]["main"];
    weatherDescription = String(desc);
    Serial.printf("[WEATHER] Description: %s\n", weatherDescription.c_str());
  } else {
    Serial.println(F("[WEATHER] Weather description not found in JSON payload"));
  }
  weatherFetched = true;
}

void setup() {
  Serial.begin(74880);
  Serial.println();
  Serial.println(F("[SETUP] Starting setup..."));
  P.begin();
  P.setFont(mFactory); // Custom font
  loadConfig(); // Load config before setting intensity & flip
  P.setIntensity(brightness);
  P.setZoneEffect(0, flipDisplay, PA_FLIP_UD);
  P.setZoneEffect(0, flipDisplay, PA_FLIP_LR);
  Serial.println(F("[SETUP] Parola (LED Matrix) initialized"));
  connectWiFi();
  Serial.println(F("[SETUP] Wifi connected"));  
  setupWebServer();
  Serial.println(F("[SETUP] Webserver setup complete"));  
  Serial.println(F("[SETUP] Setup complete"));
  Serial.println();
  printConfigToSerial();
  setupTime(); // Start NTP sync process
  displayMode = 0;
  lastSwitch = millis();
  lastColonBlink = millis();
}

void loop() {

  // --- AP Mode Animation ---
  static unsigned long apAnimTimer = 0;
  static int apAnimFrame = 0;
  if (isAPMode) {
    unsigned long now = millis();
    if (now - apAnimTimer > 750) {
      apAnimTimer = now;
      apAnimFrame++;
    }
    P.setTextAlignment(PA_CENTER);
    switch (apAnimFrame % 3) {
      case 0: P.print("AP©"); break;
      case 1: P.print("APª"); break;
      case 2: P.print("AP«"); break;
    }
    yield(); // Let system do background work
    return;  // Don't run normal display logic
  }

  static bool colonVisible = true;
  const unsigned long colonBlinkInterval = 800;
  if (millis() - lastColonBlink > colonBlinkInterval) {
    colonVisible = !colonVisible;
    lastColonBlink = millis();
  }

  static unsigned long ntpAnimTimer = 0;
  static int ntpAnimFrame = 0;
  static bool tzSetAfterSync = false;
  static time_t lastPrint = 0;

  // WEATHER FETCH
  static unsigned long lastFetch = 0;
  const unsigned long fetchInterval = 300000; // 5 minutes

  // NTP state machine
  switch (ntpState) {
    case NTP_IDLE:
      break;
    case NTP_SYNCING: {
      time_t now = time(nullptr);
      if (now > 1000) {
        Serial.println(F("\n[TIME] NTP sync successful."));
        ntpSyncSuccessful = true;
        ntpState = NTP_SUCCESS;
      } else if (millis() - ntpStartTime > ntpTimeout || ntpRetryCount > maxNtpRetries) {
        Serial.println(F("\n[TIME] NTP sync failed."));
        ntpSyncSuccessful = false;
        ntpState = NTP_FAILED;
      } else {
        if (millis() - ntpStartTime > (ntpRetryCount * 1000)) {
          Serial.print(".");
          ntpRetryCount++;
        }
      }
      break;
    }
    case NTP_SUCCESS:
      if (!tzSetAfterSync) {
        const char* posixTz = ianaToPosix(timeZone);
        setenv("TZ", posixTz, 1);
        tzset();
        tzSetAfterSync = true;
      }
      ntpAnimTimer = 0;
      ntpAnimFrame = 0;
      break;
    case NTP_FAILED:
      ntpAnimTimer = 0;
      ntpAnimFrame = 0;
      break;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    if (!weatherFetchInitiated) {
        weatherFetchInitiated = true;
        fetchWeather();
        lastFetch = millis();
    }
    if (millis() - lastFetch > fetchInterval) {
        Serial.println(F("[LOOP] Fetching weather data..."));
        weatherFetched = false;
        fetchWeather();
        lastFetch = millis();
    }
  } else {
    weatherFetchInitiated = false;
  }

  // Time display logic
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  int dayOfWeek = timeinfo.tm_wday;
  char* daySymbol = daysOfTheWeek[dayOfWeek];

  char timeStr[9]; // enough for "12:34 AM"
  if (twelveHourToggle) {
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    sprintf(timeStr, "%d:%02d", hour12, timeinfo.tm_min);
  } else {
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }

  // Only prepend day symbol if showDayOfWeek is true
  String formattedTime;
  if (showDayOfWeek) {
    formattedTime = String(daySymbol) + " " + String(timeStr);
  } else {
    formattedTime = String(timeStr);
  }

  unsigned long displayDuration = (displayMode == 0) ? clockDuration : weatherDuration;
  if (millis() - lastSwitch > displayDuration) {
    displayMode = (displayMode + 1) % 2;
    lastSwitch = millis();
    Serial.printf("[LOOP] Switching to display mode: %s\n", displayMode == 0 ? "CLOCK" : "WEATHER");
  }

  P.setTextAlignment(PA_CENTER);
  static bool weatherWasAvailable = false;

  if (displayMode == 0) { // Clock
    if (ntpState == NTP_SYNCING) {
      if (millis() - ntpAnimTimer > 750) {
        ntpAnimTimer = millis();
        switch (ntpAnimFrame % 3) {
          case 0: P.print("sync®"); break;
          case 1: P.print("sync¯"); break;
          case 2: P.print("sync°"); break;
        }
        ntpAnimFrame++;
      }
    } else if (!ntpSyncSuccessful) {
      P.print("no ntp");
    } else {
      String timeString = formattedTime;
      if (!colonVisible) timeString.replace(":", " ");
      P.print(timeString);
    }
  } else { // Weather mode
    if (weatherAvailable) {
      // --- Weather display string with humidity toggle and 99% cap ---
      String weatherDisplay;
      if (showHumidity && currentHumidity != -1) {
        int cappedHumidity = (currentHumidity > 99) ? 99 : currentHumidity;
        weatherDisplay = currentTemp + " " + String(cappedHumidity) + "%";
      } else {
        weatherDisplay = currentTemp + tempSymbol;
      }
      P.print(weatherDisplay.c_str());
      weatherWasAvailable = true;
    } else {
      if (weatherWasAvailable) {
        Serial.println(F("[DISPLAY] Weather not available, showing clock..."));
        weatherWasAvailable = false;
      }
      if (ntpSyncSuccessful) {
        String timeString = formattedTime;
        if (!colonVisible) timeString.replace(":", " ");
        P.print(timeString);
      } else {
        P.print("no temp");
      }
    }
  }

  static unsigned long lastDisplayUpdate = 0;
  const unsigned long displayUpdateInterval = 50;
  if (millis() - lastDisplayUpdate >= displayUpdateInterval) {
    lastDisplayUpdate = millis();
  }

  yield();
}