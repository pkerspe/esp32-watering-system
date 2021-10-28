#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include <SPIFFS.h>
#include <list>
#include <ArduinoJson.h>
#include <time.h>

const char *wifiSSID = "";
const char *wifiPassword = "";

const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;

const char *configFilename = "/config.json";
struct ValveConfiguration
{
  int id = 0;
  String valveName = "NA";
  String description = "NA";
  int waterDurationSec = 1;
};

#define valveCounter 4
ValveConfiguration valveConfigurations[valveCounter];

struct activationQueueEntry
{
  unsigned int ioPin;
  unsigned int duration;
  unsigned long activationEndTs = 0;
};

std::list<activationQueueEntry> activationQueItemList;

#define Channel_5_PIN 12
#define Channel_VALVE4_PIN 12

#define Channel_4_PIN 14
#define Channel_VALVE3_PIN 14

#define Channel_3_PIN 27
#define Channel_VALVE2_PIN 27

#define Channel_2_PIN 26
#define Channel_VALVE1_PIN 26

#define Channel_1_PIN 25
#define Channel_PUMP_PIN 25

byte i2cDevicesFound[10];
AsyncWebServer server(80);
AsyncWebSocket *webSockerServer;

long nextWaterBurst = 0;

void printLocalTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}

void addToActivationQueue(int ioPin, int duration)
{
  activationQueueEntry entry;
  entry.duration = duration;
  entry.ioPin = ioPin;
  activationQueItemList.push_back(entry);
  Serial.printf("Added IO Pin %i to activation queue for activation duration of %i seconds, Queue size is now %i\n", ioPin, duration, activationQueItemList.size());
}

void processQueuedActivationItems()
{
  // check if there is an active item
  if (activationQueItemList.size() > 0 && activationQueItemList.front().activationEndTs > 0)
  {
    // check if activation time is over, if so, disable pin and also disable pump
    if (millis() > activationQueItemList.front().activationEndTs)
    {
      Serial.printf("Queued activation entry has reached end ts, will disable channel-pin %i\n", activationQueItemList.front().ioPin);
      digitalWrite(Channel_PUMP_PIN, LOW);
      digitalWrite(activationQueItemList.front().ioPin, LOW);
      // remove from queue since it is done
      activationQueItemList.pop_front();
    }
    // activation time not yet over, so we don't do anything
  }
  else if (activationQueItemList.size() > 0)
  {
    // currently no item is active, so we activate the next one by setting an end timestamp and setting pins high
    activationQueItemList.front().activationEndTs = millis() + (activationQueItemList.front().duration * 1000);
    digitalWrite(Channel_PUMP_PIN, HIGH);
    digitalWrite(activationQueItemList.front().ioPin, HIGH);
    Serial.printf("Queued activation entry started will enable channel-pin %i\n", activationQueItemList.front().ioPin);
  }
}

void handleNotFound(AsyncWebServerRequest *request)
{
  request->send(404, "text/plain", "404: Not Found");
}

void readConfigFromSpiffs()
{
  SPIFFS.begin();
  File configFile = SPIFFS.open(configFilename);
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  if (error)
  {
    Serial.println(F("Failed to read file, using default configuration"));
  }

  JsonArray valves = doc["valves"].as<JsonArray>();
  int i = 0;
  for (JsonObject valveConfigEntry : valves)
  {
    valveConfigurations[i].id = valveConfigEntry["id"].as<int>();
    valveConfigurations[i].valveName = valveConfigEntry["name"].as<String>();
    valveConfigurations[i].description = valveConfigEntry["description"].as<String>();
    valveConfigurations[i].waterDurationSec = valveConfigEntry["waterDurationSec"].as<int>();
    i++;
  }

  configFile.close();
  Serial.printf("Configuration read from SPIFFS, found %i valve configuration entries\n", i);
}

void writeConfigToSpiffs()
{
  SPIFFS.remove(configFilename); // delete old file first
  File file = SPIFFS.open(configFilename, FILE_WRITE);
  if (!file)
  {
    Serial.println(F("Failed to open config file for writing"));
    return;
  }

  StaticJsonDocument<1024> doc;
  JsonArray array = doc["valves"].to<JsonArray>();
  for (int i = 0; i < valveCounter; i++)
  {
    JsonObject valveConfigEntry = array.createNestedObject();
    valveConfigEntry["id"] = valveConfigurations[i].id;
    valveConfigEntry["name"] = valveConfigurations[i].valveName;
    valveConfigEntry["description"] = valveConfigurations[i].description;
    valveConfigEntry["waterDurationSec"] = valveConfigurations[i].waterDurationSec;
  }

  if (serializeJson(doc, file) == 0)
  {
    Serial.println(F("Failed to write to file"));
  }
  else
  {
    Serial.println(F("Configuration written to SPIFFS"));
  }
  file.close();
}

void onSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    // client connected
    Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    client->ping();
  }
  else if (type == WS_EVT_DISCONNECT)
  {
    // client disconnected
    Serial.printf("ws[%s][%u] disconnect\n", server->url(), client->id());
  }
  else if (type == WS_EVT_ERROR)
  {
    // error was received from the other end
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
  }
  else if (type == WS_EVT_PONG)
  {
    // pong message was received (in response to a ping request maybe)
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
  }
  else if (type == WS_EVT_DATA)
  {
    // data packet
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len)
    {
      // the whole message is in a single frame and we got all of it's data
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);
      if (info->opcode == WS_TEXT)
      {
        data[len] = 0;
        Serial.printf("%s\n", (char *)data);
      }
      else
      {
        for (size_t i = 0; i < info->len; i++)
        {
          Serial.printf("%02x ", data[i]);
        }
        Serial.printf("\n");
      }
      // if (info->opcode == WS_TEXT)
      //   client->text("I got your text message");
      // else
      //   client->binary("I got your binary message");
    }
    else
    {
      // message is comprised of multiple frames or the frame is split into multiple packets
      if (info->index == 0)
      {
        if (info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }

      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);
      if (info->message_opcode == WS_TEXT)
      {
        data[len] = 0;
        Serial.printf("%s\n", (char *)data);
      }
      else
      {
        for (size_t i = 0; i < len; i++)
        {
          Serial.printf("%02x ", data[i]);
        }
        Serial.printf("\n");
      }

      if ((info->index + len) == info->len)
      {
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if (info->final)
        {
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
          if (info->message_opcode == WS_TEXT)
            client->text("I got your text message");
          else
            client->binary("I got your binary message");
        }
      }
    }
  }
}

void initWifi(){
  WiFi.hostname("Water-Controller");
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSSID, wifiPassword);
  int timeoutCounter = 0;
  Serial.print("Connecting to wifi");
  while (WiFi.status() != WL_CONNECTED && timeoutCounter < 15)
  { // Wait for the Wi-Fi to connect
    delay(500);
    Serial.print('.');
    timeoutCounter++;
  }
  Serial.println("");

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Failed to connect to wifi network or non configured");
  }
  else
  {
    Serial.printf("Connected to WiFi\nIP address: %s\n", WiFi.localIP().toString().c_str());
  }
}

void startWebserver()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/index.html", String(), false); });

  server.on("/scheduledDuration", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (request->hasParam("channel", true) && request->hasParam("duration", true)){
      AsyncWebParameter *channelParam = request->getParam("channel", true, false);
      int channel = String(channelParam->value().c_str()).toInt();

      AsyncWebParameter *durationParam = request->getParam("duration", true, false);
      int duration = String(durationParam->value().c_str()).toInt();

      if(channel > 0 && channel <= valveCounter && duration >= 0 && duration <= 120){
        Serial.printf("Setting valve %i to scheduled duration of %i seconds\n", channel, duration);
        valveConfigurations[channel-1].waterDurationSec = duration;
        writeConfigToSpiffs();
        request->send(203);  
      } else {
        request->send(400);
      }
    } });

  server.on("/triggerChannel", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              if (request->hasParam("channel", true) && request->hasParam("duration", true))
              {
                AsyncWebParameter *channelParam = request->getParam("channel", true, false);
                int channel = String(channelParam->value().c_str()).toInt();

                AsyncWebParameter *durationParam = request->getParam("duration", true, false);
                int duration = String(durationParam->value().c_str()).toInt();

                //check for valid channels and trigger acitvation if needed with timeout
                switch (channel)
                {
                case 1:
                  addToActivationQueue(Channel_VALVE1_PIN, duration);
                  break;

                case 2:
                  addToActivationQueue(Channel_VALVE2_PIN, duration);
                  break;

                case 3:
                  addToActivationQueue(Channel_VALVE3_PIN, duration);
                  break;

                case 4:
                  addToActivationQueue(Channel_VALVE4_PIN, duration);
                  break;

                default:
                  request->send(400, "application/json", "{\"error\": \"Invalid channel number given, valid values are 1 to 4\"}");
                  return;
                }

                char buffer[150];
                sprintf(buffer, "{\"channel\": %i , \"duration\": %i}", channel, duration);
                request->send(200, "application/json", buffer);
              }
              else
              {
                request->send(400, "application/json", "{\"error\": \"missing POST parameter. channel and duration are required parameters\"}");
              } });

  server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              int pumpStatus = digitalRead(Channel_PUMP_PIN);
              int valve1Status = digitalRead(Channel_VALVE1_PIN);
              int valve2Status = digitalRead(Channel_VALVE2_PIN);
              int valve3Status = digitalRead(Channel_VALVE3_PIN);
              int valve4Status = digitalRead(Channel_VALVE4_PIN);
              long timeToNextBurst = (nextWaterBurst - millis())/1000;
              char buffer[150];
              tm timeinfo;
              if (!getLocalTime(&timeinfo)){
                Serial.println("Failed to read time");
              }
              
              sprintf(buffer, "{\"status\": {\"nextPlannedRun\":  %ld, \"pump\": %i, \"valve1\": %i, \"valve2\": %i, \"valve3\": %i, \"valve4\": %i}, \"config\": {\"servertime\": \"%02i:%02i:%02i\"}}", timeToNextBurst, pumpStatus, valve1Status, valve2Status, valve3Status, valve4Status, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
              request->send(200, "application/json", buffer); });

  server.on("/configuration.json", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, configFilename, "application/json"); });

  // serve other files from SPIFFS
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  server.onNotFound(handleNotFound);
  AsyncElegantOTA.begin(&server);
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // init socket connection stuff
  webSockerServer = new AsyncWebSocket("/ws");
  webSockerServer->onEvent(onSocketEvent);
  server.addHandler(webSockerServer);

  server.begin();
  Serial.printf("HTTP server started, webinterface available at: http://%s/\n", WiFi.localIP().toString().c_str());
}

long lastSocketCleanup = 0;
long nextSocketPackageToSend = 0;

void performWebSocketTasks()
{
  long ts = millis();
  if (lastSocketCleanup < ts - 5000)
  {
    webSockerServer->cleanupClients(); // cleaning stale websocket connections to free ressources
    lastSocketCleanup = ts;
  }
  if (ts > nextSocketPackageToSend && webSockerServer->count() > 0)
  {
    nextSocketPackageToSend = ts + 500;
    uint8_t statusBitset = 0;
    statusBitset |= digitalRead(Channel_PUMP_PIN) << 4;
    statusBitset |= digitalRead(Channel_VALVE1_PIN) << 3;
    statusBitset |= digitalRead(Channel_VALVE2_PIN) << 2;
    statusBitset |= digitalRead(Channel_VALVE3_PIN) << 1;
    statusBitset |= digitalRead(Channel_VALVE4_PIN) << 0;
    char buffer[50];
    sprintf(buffer, "{\"cs\": %i}", statusBitset);
    webSockerServer->textAll(buffer);
  }
}

void setup()
{
  Serial.begin(115200);
  initWifi();
  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
  }

  readConfigFromSpiffs();

  startWebserver();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Time set from NTPD Server: ");
  printLocalTime();

  pinMode(Channel_5_PIN, OUTPUT);
  pinMode(Channel_4_PIN, OUTPUT);
  pinMode(Channel_3_PIN, OUTPUT);
  pinMode(Channel_2_PIN, OUTPUT);
  pinMode(Channel_1_PIN, OUTPUT);

  digitalWrite(Channel_5_PIN, LOW);
  digitalWrite(Channel_4_PIN, LOW);
  digitalWrite(Channel_3_PIN, LOW);
  digitalWrite(Channel_2_PIN, LOW);
  digitalWrite(Channel_1_PIN, LOW);
}

void conductPlannedWaterBurst()
{
  // PUMP on (turn on bit earlier to reduce current spikes and build pressure)
  digitalWrite(Channel_PUMP_PIN, HIGH);
  delay(500);

  digitalWrite(Channel_2_PIN, HIGH); // Wein
  delay(valveConfigurations[0].waterDurationSec * 1000);

  digitalWrite(Channel_2_PIN, LOW);
  digitalWrite(Channel_3_PIN, HIGH); // Blauregen 1
  delay(valveConfigurations[1].waterDurationSec * 1000);

  digitalWrite(Channel_3_PIN, LOW);
  digitalWrite(Channel_4_PIN, HIGH); // Blauregen 2
  delay(valveConfigurations[2].waterDurationSec * 1000);

  digitalWrite(Channel_4_PIN, LOW);
  digitalWrite(Channel_5_PIN, HIGH); // Buchsbaum + Blauregen 3
  delay(valveConfigurations[3].waterDurationSec * 1000);

  digitalWrite(Channel_5_PIN, LOW);
  // PUMP off
  digitalWrite(Channel_PUMP_PIN, LOW);
}

void loop()
{
  if (nextWaterBurst < millis())
  {
    nextWaterBurst = millis() + (60000 * 60 * 12);
    conductPlannedWaterBurst();
  }

  processQueuedActivationItems();

  performWebSocketTasks();
}
