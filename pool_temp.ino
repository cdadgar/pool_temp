/*
 * module is a WeMos D1 R1
 * flash size set to 4M (1M SPIFFS) or 4MB (FS:1MB OTA:~1019KB) (latest esp board sw uses this)
 * OR
 * module is a esp12e
 * flash size set to 4M (1M SPIFFS) or 4MB (FS:1MB OTA:~1019KB) (latest esp board sw uses this)
 * 
 */

/*
 * todo:
 */

/*
 * library sources:
 * ESP8266WiFi, ESP8266WebServer, FS, DNSServer, Hash, EEPROM, ArduinoOTA - https://github.com/esp8266/Arduino
 * WebSocketsServer - https://github.com/Links2004/arduinoWebSockets (git)
 * WiFiManager - https://github.com/tzapu/WiFiManager (git)
 * ESPAsyncTCP - https://github.com/me-no-dev/ESPAsyncTCP (git)
 * ESPAsyncUDP - https://github.com/me-no-dev/ESPAsyncUDP (git)
 * OneWire - https://github.com/PaulStoffregen/OneWire (git)
 * DallasTemperature - https://github.com/milesburton/Arduino-Temperature-Control-Library (git)
 * PubSub - https://github.com/knolleary/pubsubclient (git)
 * TimeLib - https://github.com/PaulStoffregen/Time (git)
 * Timezone - https://github.com/JChristensen/Timezone (git)
 * ArduinoJson - https://github.com/bblanchon/ArduinoJson  (git)
 */

#include <ESP8266WiFi.h>
#include <WebSocketsServer.h>
#include <Hash.h>
#include <TimeLib.h> 
#include <Timezone.h>

//US Eastern Time Zone (New York, Detroit)
TimeChangeRule myDST = {"EDT", Second, Sun, Mar, 2, -240};    //Daylight time = UTC - 4 hours
TimeChangeRule mySTD = {"EST", First, Sun, Nov, 2, -300};     //Standard time = UTC - 5 hours
Timezone myTZ(myDST, mySTD);

// --------------------------------------------

// web server library includes
#include <ESP8266WebServer.h>
//#include <ArduinoJson.h>
#include <EEPROM.h>

// --------------------------------------------

// file system (spiffs) library includes
#include <FS.h>

// --------------------------------------------

// wifi manager library includes
#include <DNSServer.h>
#include <WiFiManager.h>

// --------------------------------------------

// aync library includes
#include <ESPAsyncTCP.h>
#include <ESPAsyncUDP.h>

// --------------------------------------------

WiFiManager wifiManager;
String ssid;

// add defines if we're not using a wemos board
#ifndef D6
#define D6 12
#endif




// --------------------------------------------

// ds18b20 temperture sensor library includes
#include <OneWire.h> 
#include <DallasTemperature.h>

// --------------------------------------------

// mqtt library includes
#include <PubSubClient.h>

// --------------------------------------------

// arduino ota library includes
#include <ArduinoOTA.h>

#include <WiFiClient.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
ESP8266HTTPUpdateServer httpUpdater;

// --------------------------------------------


ESP8266WebServer server(80);
File fsUploadFile;
bool isSetup = false;
WebSocketsServer webSocket = WebSocketsServer(81);
int webClient = -1;
int setupClient = -1;

// temperature
#define ONE_WIRE_BUS D6
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress thermometer_spa, thermometer_pool;
unsigned long lastTempRequest = 0;
int delayInMillis = 0;
#define resolution 12
#define TEMP_ERROR -999
float lastTempSpa = TEMP_ERROR;
float lastTempPool = TEMP_ERROR;

bool isTimeSet = false;
unsigned long lastMinutes;

WiFiClient espClient;
PubSubClient client(espClient);

#define HOST_NAME "POOL_TEMP"
#define MQTT_IP_ADDR "192.168.1.210"
#define MQTT_IP_PORT 1883

bool isPromModified;
bool isMemoryReset = false;
//bool isMemoryReset = true;

typedef struct {
  char host_name[17];
  char mqtt_ip_addr[17];
  int mqtt_ip_port;
  byte use_mqtt;
} configType;

configType config;


void setup() {
  // start serial port
  Serial.begin(115200);
  Serial.print(F("\n\n"));

  Serial.println(F("esp8266 POOL_TEMP"));
  Serial.println(F("compiled:"));
  Serial.print( __DATE__);
  Serial.print(F(","));
  Serial.println( __TIME__);

  // must specify amount of eeprom to use (max is 4k?)
  EEPROM.begin(4*1024);
  
  loadConfig();
  isMemoryReset = false;

  if (!setupTemperature()) {
//    return;
  }
    
  if (!setupWifi())
    return;

  setupTime();
  setupWebServer();  
  setupMqtt();
  setupOta();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  lastMinutes = 0;

  isSetup = true;
}


#define MAGIC_NUM   0xAC

#define MAGIC_NUM_ADDRESS      0
#define CONFIG_ADDRESS         1
#define CODE_SIZE_ADDRESS      CONFIG_ADDRESS + sizeof(config)

void set(char *name, const char *value) {
  for (int i=strlen(value); i >= 0; --i)
    *(name++) = *(value++);
}

void loadConfig(void) {
  int magicNum = EEPROM.read(MAGIC_NUM_ADDRESS);
  if (magicNum != MAGIC_NUM) {
    Serial.println(F("invalid eeprom data"));
    isMemoryReset = true;
  }
  
  if (isMemoryReset) {
    // nothing saved in eeprom, use defaults
    Serial.println(F("using default config"));
    set(config.host_name, HOST_NAME);
    set(config.mqtt_ip_addr, MQTT_IP_ADDR);
    config.mqtt_ip_port = MQTT_IP_PORT;
    config.use_mqtt = 0;

    saveConfig();
  }
  else {
    int addr = CONFIG_ADDRESS;
    byte *ptr = (byte *)&config;
    for (int i=0; i < sizeof(config); ++i, ++ptr)
      *ptr = EEPROM.read(addr++);
  }

  Serial.printf("host_name %s\n", config.host_name);
  Serial.printf("use_mqtt %d\n", config.use_mqtt);
  Serial.printf("mqqt_ip_addr %s\n", config.mqtt_ip_addr);
  Serial.printf("mqtt_ip_port %d\n", config.mqtt_ip_port);
}

void saveConfig(void) {
  isPromModified = false;
  update(MAGIC_NUM_ADDRESS, MAGIC_NUM);

  byte *ptr = (byte *)&config;
  int addr = CONFIG_ADDRESS;
  for (int j=0; j < sizeof(config); ++j, ++ptr)
    update(addr++, *ptr);
  
  if (isPromModified)
    EEPROM.commit();
}


void update(int addr, byte data) {
  if (EEPROM.read(addr) != data) {
    EEPROM.write(addr, data);
    isPromModified = true;
  }
}


void setupOta(void) {
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(config.host_name);

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    const char *msg = "Unknown Error";
    if (error == OTA_AUTH_ERROR) {
      msg = "Auth Failed";
    } else if (error == OTA_BEGIN_ERROR) {
      msg = "Begin Failed";
    } else if (error == OTA_CONNECT_ERROR) {
      msg = "Connect Failed";
    } else if (error == OTA_RECEIVE_ERROR) {
      msg = "Receive Failed";
    } else if (error == OTA_END_ERROR) {
      msg = "End Failed";
    }
    Serial.println(msg);
  });
  
  ArduinoOTA.begin();
  Serial.println("Arduino OTA ready");

  char host[20];
  sprintf(host, "%s-webupdate", config.host_name);
  MDNS.begin(host);
  httpUpdater.setup(&server);
  MDNS.addService("http", "tcp", 80);
  Serial.println("Web OTA ready");
}


void setupMqtt() {
  client.setServer(config.mqtt_ip_addr, config.mqtt_ip_port);
  client.setCallback(callback);
}


void loop() {
  if (!isSetup)
    return;

  unsigned long time = millis();

  checkTime(time);
  checkTemperature(time);

  webSocket.loop();
  server.handleClient();
  MDNS.update();
 
  // mqtt
  if (config.use_mqtt) {
    if (!client.connected()) {
      reconnect();
    }
    client.loop();
  }

  ArduinoOTA.handle();
}


void checkTime(unsigned long time) {
  int minutes = minute();

  if (minutes == lastMinutes)
    return;

  // resync time at 3am every morning
  // this also catches daylight savings time changes which happen at 2am
  if (minutes == 0 && hour() == 3)
    isTimeSet = false;

  if (!isTimeSet)
    setupTime();
 
  lastMinutes = minutes;
  printTime();
}


const char *weekdayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};


void printTime() {
  if (webClient == -1)
    return;
    
  int dayOfWeek = weekday()-1;
  int hours = hour();
  int minutes = minute();
  const char *ampm = "a";
  int h = hours;
  if (hours == 0)
    h = 12;
  else if (h == 12)
    ampm = "p";
  else if (hours > 12) {
    h -= 12;
    ampm = "p";
  }
  
  char buf[10];
  sprintf(buf, "  %2d:%02d%s", h, minutes, ampm); 
//  Serial.println(buf);

  char msg[6+1+4];
  sprintf(msg, "%s %s", buf, weekdayNames[dayOfWeek]); 
  sendWeb("time", msg);
}


void printName() {
  if (webClient == -1)
    return;
    
  sendWeb("name", config.host_name);
}


void configModeCallback(WiFiManager *myWiFiManager) {
  // this callback gets called when the enter AP mode, and the users
  // need to connect to us in order to configure the wifi
  Serial.print(F("Join:"));
  Serial.println(config.host_name);
  Serial.print(F("Goto:"));
  Serial.println(WiFi.softAPIP());
}


bool setupTemperature(void) {
  sensors.begin();
  if (sensors.getDeviceCount() != 2) {
    Serial.println("Unable to locate both temperature sensors");
    return false;
  }
  if (!sensors.getAddress(thermometer_spa, 0)) {
    Serial.println("Unable to find address for spa temperature sensor"); 
    return false;
  }
  if (!sensors.getAddress(thermometer_pool, 1)) {
    Serial.println("Unable to find address for pool temperature sensor"); 
    return false;
  }
  sensors.setResolution(thermometer_spa, resolution);
  sensors.setResolution(thermometer_pool, resolution);
  
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();
  delayInMillis = 750 / (1 << (12 - resolution)); 
  lastTempRequest = millis(); 

  return true;
}

void checkTemperature(unsigned long time) {
  if (time - lastTempRequest >= delayInMillis) // waited long enough??
  {
//  Serial.println(F("checking temperature"));
    checkTemperature();
        
    sensors.requestTemperatures(); 
    lastTempRequest = millis(); 
  }
}

void checkTemperature(void) {
  float tempF = sensors.getTempF(thermometer_spa);
  tempF = (float)round(tempF*10)/10.0;
  if ( tempF != lastTempSpa ) {
    lastTempSpa = tempF;

//    Serial.print("Spa Temperature is ");
//    Serial.println(tempF);

    printCurrentTemperature("spa", lastTempSpa);
  }

  tempF = sensors.getTempF(thermometer_pool);
  tempF = (float)round(tempF*10)/10.0;
  if ( tempF != lastTempPool ) {
    lastTempPool = tempF;

//    Serial.print("Pool Temperature is ");
//    Serial.println(tempF);

    printCurrentTemperature("pool", lastTempPool);
  }
}

bool setupWifi(void) {
  WiFi.hostname(config.host_name);
  
//  wifiManager.setDebugOutput(false);
  
  //reset settings - for testing
  //wifiManager.resetSettings();

  ssid = WiFi.SSID();
  if (ssid.length() > 0) {
    Serial.print(F("Connecting to "));
    Serial.println(ssid);
  }
  
  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);

  if(!wifiManager.autoConnect(config.host_name)) {
    Serial.println(F("failed to connect and hit timeout"));
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  } 

  return true;
}


void setupTime(void) {
  Serial.println(F("Getting time"));

  AsyncUDP* udp = new AsyncUDP();

  // time.nist.gov NTP server
  // NTP requests are to port 123
  if (udp->connect(IPAddress(129,6,15,28), 123)) {
//    Serial.println("UDP connected");
    
    udp->onPacket([](void *arg, AsyncUDPPacket packet) {
//      Serial.println(F("received NTP packet"));
      byte *buf = packet.data();
      
      //the timestamp starts at byte 40 of the received packet and is four bytes,
      // or two words, long. First, esxtract the two words:
    
      // convert four bytes starting at location 40 to a long integer
      unsigned long secsSince1900 =  (unsigned long)buf[40] << 24;
      secsSince1900 |= (unsigned long)buf[41] << 16;
      secsSince1900 |= (unsigned long)buf[42] << 8;
      secsSince1900 |= (unsigned long)buf[43];
      time_t utc = secsSince1900 - 2208988800UL;
    
      TimeChangeRule *tcr;
      time_t local = myTZ.toLocal(utc, &tcr);
      Serial.printf("\ntime zone %s\n", tcr->abbrev);
    
      setTime(local);
    
      // just print out the time
      printTime();
    
      isTimeSet = true;

      free(arg);
    }, udp);
    
//    Serial.println(F("sending NTP packet"));

    const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
    byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold outgoing packet

    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12]  = 49;
    packetBuffer[13]  = 0x4E;
    packetBuffer[14]  = 49;
    packetBuffer[15]  = 52;
    
    // all NTP fields have been given values, now
    // send a packet requesting a timestamp:
    udp->write(packetBuffer, NTP_PACKET_SIZE);
  }
  else {
    free(udp);
    Serial.println(F("\nWiFi:time failed...will retry in a minute"));
  }
}


void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      if (num == webClient)
        webClient = -1;
      else if (num == setupClient)
        setupClient = -1;
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
      }      

      if (strcmp((char *)payload,"/") == 0) {
        webClient = num;
      
        // send the current state
        printName();
        printCurrentTemperature("spa", lastTempSpa);
        printCurrentTemperature("pool", lastTempPool);
        printTime();
      }
      else if (strcmp((char *)payload,"/setup") == 0) {
        setupClient = num;

        char json[256];
        strcpy(json, "{");
        sprintf(json+strlen(json), "\"date\":\"%s\"", __DATE__);
        sprintf(json+strlen(json), ",\"time\":\"%s\"", __TIME__);
        sprintf(json+strlen(json), ",\"host_name\":\"%s\"", config.host_name);
        sprintf(json+strlen(json), ",\"use_mqtt\":\"%d\"", config.use_mqtt);
        sprintf(json+strlen(json), ",\"mqtt_ip_addr\":\"%s\"", config.mqtt_ip_addr);
        sprintf(json+strlen(json), ",\"mqtt_ip_port\":\"%d\"", config.mqtt_ip_port);
        sprintf(json+strlen(json), ",\"ssid\":\"%s\"", ssid.c_str());
        strcpy(json+strlen(json), "}");
//        Serial.printf("len %d\n", strlen(json));
        webSocket.sendTXT(setupClient, json, strlen(json));
      }
      else {
        Serial.printf("unknown call %s\n", payload);
      }
      break;
    case WStype_TEXT:
      Serial.printf("[%u] get Text: %s\n", num, payload);

      if (num == webClient) {
        // no commands on main page
      }
      else if (num == setupClient) {
        const char *target = "command";
        char *ptr = strstr((char *)payload, target) + strlen(target)+3;
        if (strncmp(ptr,"reboot",6) == 0) {
          ESP.restart();
        }
        else if (strncmp(ptr,"wifi",4) == 0) {
          wifiManager.resetSettings();
          ESP.restart();
        }
        else if (strncmp(ptr,"save",4) == 0) {
          Serial.printf("save setup\n");
          
          const char *target = "host_name";
          char *ptr = strstr((char *)payload, target) + strlen(target)+3;
          char *end = strchr(ptr, '\"');
          memcpy(config.host_name, ptr, (end-ptr));
          config.host_name[end-ptr] = '\0';

          target = "use_mqtt";
          ptr = strstr((char *)payload, target) + strlen(target)+3;
          config.use_mqtt = strtol(ptr, &ptr, 10);

          target = "mqtt_ip_addr";
          ptr = strstr((char *)payload, target) + strlen(target)+3;
          end = strchr(ptr, '\"');
          memcpy(config.mqtt_ip_addr, ptr, (end-ptr));
          config.mqtt_ip_addr[end-ptr] = '\0';

          target = "mqtt_ip_port";
          ptr = strstr((char *)payload, target) + strlen(target)+3;
          config.mqtt_ip_port = strtol(ptr, &ptr, 10);

          Serial.printf("host_name %s\n", config.host_name);
          Serial.printf("use_mqtt %d\n", config.use_mqtt);
          Serial.printf("mqtt_ip_addr %s\n", config.mqtt_ip_addr);
          Serial.printf("mqtt_ip_port %d\n", config.mqtt_ip_port);
          saveConfig();
        }
      }
      break;
  }
}


void sendWeb(const char *command, const char *value) {
  send(webClient, command, value);
}


void send(int client, const char *command, const char *value) {
  char json[128];
  sprintf(json, "{\"command\":\"%s\",\"value\":\"%s\"}", command, value);
  webSocket.sendTXT(client, json, strlen(json));
}


void printCurrentTemperature(const char *item, float temp) {
  char buf[7];
  dtostrf(temp, 4, 1, buf);

  if (webClient != -1) {
    sendWeb(item, buf);  
  }
  
  // mqtt
  if (config.use_mqtt) {
    char topic[40];
    sprintf(topic, "%s/%s/temperature", config.host_name, item);
    client.publish(topic, buf);
  }
}


//format bytes
String formatBytes(size_t bytes){
  if (bytes < 1024){
    return String(bytes)+"B";
  } else if(bytes < (1024 * 1024)){
    return String(bytes/1024.0)+"KB";
  } else if(bytes < (1024 * 1024 * 1024)){
    return String(bytes/1024.0/1024.0)+"MB";
  } else {
    return String(bytes/1024.0/1024.0/1024.0)+"GB";
  }
}


String getContentType(String filename){
  if(server.hasArg("download")) return "application/octet-stream";
  else if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  else if(filename.endsWith(".xml")) return "text/xml";
  else if(filename.endsWith(".pdf")) return "application/x-pdf";
  else if(filename.endsWith(".zip")) return "application/x-zip";
  else if(filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}


bool handleFileRead(String path){
  Serial.println("handleFileRead: " + path);
  if(path.endsWith("/")) path += "index.htm";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if(SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)){
    if(SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}


void handleFileUpload_edit(){
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;
    Serial.print("handleFileUpload Name: "); Serial.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    //Serial.print("handleFileUpload Data: "); Serial.println(upload.currentSize);
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile)
      fsUploadFile.close();
    Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
  }
}


void handleFileDelete(){
  if(server.args() == 0) return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.println("handleFileDelete: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}


void handleFileCreate(){
  if(server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.println("handleFileCreate: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if(file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}


void handleFileList() {
  if(!server.hasArg("dir")) {server.send(500, "text/plain", "BAD ARGS"); return;}
  
  String path = server.arg("dir");
  Serial.println("handleFileList: " + path);
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while(dir.next()){
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir)?"dir":"file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }
  
  output += "]";
  server.send(200, "text/json", output);
}


void countRootFiles(void) {
  int num = 0;
  size_t totalSize = 0;
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    ++num;
    String fileName = dir.fileName();
    size_t fileSize = dir.fileSize();
    totalSize += fileSize;
    Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
  }
  Serial.printf("FS File: serving %d files, size: %s from /\n", num, formatBytes(totalSize).c_str());
}


void setupWebServer(void) {
  SPIFFS.begin();

  countRootFiles();

  // list directory
  server.on("/list", HTTP_GET, handleFileList);
  
  // load editor
  server.on("/edit", HTTP_GET, [](){
    if(!handleFileRead("/edit.htm")) server.send(404, "text/plain", "FileNotFound");
  });
  
  // create file
  server.on("/edit", HTTP_PUT, handleFileCreate);
  
  // delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);
  
  // first callback is called after the request has ended with all parsed arguments
  // second callback handles file uploads at that location
  server.on("/edit", HTTP_POST, [](){ server.send(200, "text/plain", ""); }, handleFileUpload_edit);

  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([](){
    if(!handleFileRead(server.uri()))
      server.send(404, "text/plain", "FileNotFound");
  });

  server.begin();

  Serial.println("HTTP server started");
}


void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
//    // Create a random client ID
//    String clientId = "ESP8266Client-";
//    clientId += String(random(0xffff), HEX);
//    // Attempt to connect
//    if (client.connect(clientId.c_str())) {
    if (client.connect(config.host_name)) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}


void callback(char* topic, byte* payload, unsigned int length) {
  // no callbacks expected
}
