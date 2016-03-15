//With great thanks to http://blog.nyl.io/esp8266-led-arduino/
//To use the ESP board: enter http://arduino.esp8266.com/stable/package_esp8266com_index.json in Arduino->Preferences->Additional Board Manager

//This script makes the ESP chip a Webserver, as per this example:https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266WebServer/

//We have four lights: red, orange, green and bus
//Eacht light can have several different states: on, off and blink

//Requests to the server have to be made as follows:
// protocal:adress:port/setlight?light=state&light=state ; for example:
//http://10.0.0.254:85/setlight?red=on&bus=blink&green=on&bus=blink


//@TODO:
// - Make the ESP chip aware of the current time and turn the lights OFF at certain thresholds
//   http://www.esp8266.com/viewtopic.php?p=18395 OR https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266WiFi/examples/NTPClient/NTPClient.ino
// - Make OTA updates possible
//   https://github.com/esp8266/Arduino/blob/master/doc/ota_updates/ota_updates.md

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <Time.h>

//////////////////////////////
//Adjust the variables below//
//////////////////////////////

//Hardware settings
const int pinLightRed     = 4; 
const int pinLightOrange  = 5;
const int pinLightGreen   = 12;
const int pinLightBus     = 13;

//WiFi settings
const char* host     = "esp8266-trafficlight-large";//The host name will show up in your network
const char* ssid     = "Bouwlust WiFi"; 
const char* password = "";

//Webserver settings
const int webserverPort = 85;//Default: 80
unsigned int localPortUDP = 2390;

//Interval at which lights should blink
const long blinkInterval = 1000;//Blink interval, in miliseconds

//UDP time settings
const int localTimeOffsetHour = 1;//Hour offset from UTC time
const int localTimeOffsetType = 1;//1=add, 0=substract
const int timeUpdateInterval = 60;//When we should update, in minutes

//We also use a clock function: its not possible to operate the lights if before or past htese values
//Set the hour at which to enable; before this hour, lights will stay off
const long timeStart = 8;
//Set the hour at which to disable; after this hour, lights will stay off
const long timeEnd = 23;

////////////////////////////////
//STOP editing below this line//
////////////////////////////////

//UDP variables
char timeServer[] = "nl.pool.ntp.org"; 
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE]; // buffer to hold incoming and outgoing packets
unsigned timeLastUpdated;
const int timeUpdateIntervalSec = timeUpdateInterval*60;
time_t update;

//Variables that hold the status of the lights
String redValue     = "off";
int redState        = LOW;
String orangeValue  = "off";
int orangeState     = LOW;
String greenValue   = "off";
int greenState      = LOW;
String busValue     = "off";
int busState        = LOW;

//Variables to store timing for blinking
unsigned long previousJsonMillis = 0; 
unsigned long previousBlinkMillisRed = 0; 
unsigned long previousBlinkMillisOrange = 0; 
unsigned long previousBlinkMillisGreen = 0; 
unsigned long previousBlinkMillisBus = 0; 

//Creating an instance of the webserver and udp class
ESP8266WebServer server(webserverPort);
WiFiUDP udp;

//Default input form for OTA update
const char* serverIndex = "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";

void setup(){
  Serial.begin(9600);
  delay(10);
  Serial.println("ESP8266 chip is starting, excecuting the setup function right now");
  
  pinMode(pinLightRed, OUTPUT); 
  pinMode(pinLightOrange, OUTPUT);
  pinMode(pinLightGreen, OUTPUT);
  pinMode(pinLightBus, OUTPUT);
  
  startupBlink();

  //Connecting to WiFi
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  
  int wifi_ctr = 0;
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(pinLightOrange, HIGH);
    delay(200);
    digitalWrite(pinLightOrange, LOW);
    Serial.print(".");
  }
  
  digitalWrite(pinLightGreen, HIGH);
  Serial.print("WiFi connected. Local IP is ");
  Serial.print(WiFi.localIP());
  //Serial.print(" and remote IP is ");
  //Serial.print(WiFi.remoteIP());  
  Serial.println();

  //Beginning the MDNS responder
  if (MDNS.begin(host)) {
    Serial.println("MDNS responder started");
  }else{
    Serial.println("WARNING! Failed to start MDNS");
  }

  //Setting callbacks for the http server
  server.on("/", handleRoot);
  server.on("/setlight", handleSetLight);
  server.on("/setlight/", handleSetLight);
  server.on("/getlight", handleGetLight);
  server.on("/getlight/", handleGetLight);
  server.onNotFound(handleNotFound);
  server.on("/update", HTTP_GET, [](){
    Serial.println("Handeling a GET call to the Update function");
      server.sendHeader("Connection", "close");
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "text/html", serverIndex);
    });
  server.on("/update", HTTP_POST, [](){
    Serial.println("Handeling a POST call to the Update function");
      server.sendHeader("Connection", "close");
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "text/plain", (Update.hasError())?"FAIL":"OK");
      ESP.restart();
    },[](){
      HTTPUpload& upload = server.upload();
      if(upload.status == UPLOAD_FILE_START){
        Serial.setDebugOutput(true);
        WiFiUDP::stopAll();
        Serial.printf("Update: %s\n", upload.filename.c_str());
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if(!Update.begin(maxSketchSpace)){//start with max available size
          Update.printError(Serial);
        }
      } else if(upload.status == UPLOAD_FILE_WRITE){
        if(Update.write(upload.buf, upload.currentSize) != upload.currentSize){
          Update.printError(Serial);
        }
      } else if(upload.status == UPLOAD_FILE_END){
        if(Update.end(true)){ //true to set the size to the current progress
          Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
        Serial.setDebugOutput(false);
      }
      yield();
  });

  server.begin();
  Serial.println("HTTP server started");

  // A UDP instance to let us send and receive packets over UDP
  udp.begin(localPortUDP);
  Serial.println("UDP started");

  //Starting TCP
  MDNS.addService("http", "tcp", webserverPort);

  //We're all set, let's confirm this by turning each light on/off onces; afterwards will set them to blink
  startupLoop();

  delay(1000);

  redValue      = "blink";
  orangeValue   = "blink";
  greenValue    = "blink";
  busValue      = "blink";

}//End void setup


void loop(){
  
  server.handleClient();

  unsigned long currentMillis = millis();

  if(now() > update){
    Serial.println("We have to update the NTP time");
     setTimefromUdp();
  }
  
  //Red LED
  if (redValue == "on") {
      redState = HIGH;
  }else if (redValue == "off"){
      redState = LOW;
  }else if(redValue == "blink" && (currentMillis - previousBlinkMillisRed >= blinkInterval) ){
      previousBlinkMillisRed = currentMillis;//@TODO: store for every?
      if(redState == LOW){
        redState = HIGH;
      }else{
        redState = LOW;
      }
  }

  //Orange LED
  if (orangeValue == "on") {
      orangeState = HIGH;
  }else if (orangeValue == "off"){
      orangeState = LOW;
  }else if(orangeValue == "blink" && (currentMillis - previousBlinkMillisOrange >= blinkInterval) ){
     previousBlinkMillisOrange = currentMillis;
      if(orangeState == LOW){
        orangeState = HIGH;
      }else{
        orangeState = LOW;
      }
   }

  //Green LED
  if (greenValue == "on") {
      greenState = HIGH;
  }else if (greenValue == "off"){
      greenState = LOW;
  }else if(greenValue == "blink" && (currentMillis - previousBlinkMillisGreen >= blinkInterval) ){
     previousBlinkMillisGreen = currentMillis;
      if(greenState == LOW){
          greenState = HIGH;
      }else{
          greenState = LOW;
      }
  }

  //Bus light
  if (busValue == "on") {
      busState = HIGH; 
  }else if (busValue == "off"){
      busState = LOW;
  }else if(busValue == "blink" && (currentMillis - previousBlinkMillisBus >= blinkInterval) ){
     previousBlinkMillisBus = currentMillis;
      if(busState == LOW){
          busState = HIGH;
      }else{
        busState = LOW;
      }
  }

  if(!active()){
    redState = LOW;
    orangeState = LOW;
    greenState = LOW;
    busState = LOW;
  }
  digitalWrite(pinLightRed, redState);
  digitalWrite(pinLightOrange, orangeState);
  digitalWrite(pinLightGreen, greenState);
  digitalWrite(pinLightBus, busState);
  
}//End loop

int active(){
  unsigned long h = hour();
  if(h < timeStart || h >= timeEnd ){
    Serial.println("Turn off the lights, as set in the time-settings");
    return false;
  }else{
    return true;
  }
}

//////////////////////////////////////////
//Functions for NTP time synchronisation//
//////////////////////////////////////////
unsigned long sendNTPpacket(char* address)
{
  Serial.println("running sendNTPpacket");
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now you can send a packet requesting a timestamp:
  udp.beginPacket(address, 123); //NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
} //sendNTPpacket

void setTimefromUdp() {
  Serial.println("function setTimefromUdp");
  
  time_t epoch = 0UL;
  
   if( (epoch =  getFromNTP()) != 0 ){ // get from time server
      epoch -= 2208988800UL;
      if(localTimeOffsetType == 1){
         epoch += (localTimeOffsetHour*3600);
      }else{
         epoch -= (localTimeOffsetHour*3600);
      }
      setTime(epoch += dst(epoch));
      update = now() +  timeUpdateIntervalSec; // set next update time if successful
      
      Serial.print("Time has been update, right now it's ");
      Serial.print(hour());
      Serial.print(":");
      Serial.print(minute());
      Serial.println(" uur");
   }
   else{
      update = now() + 5; // or try again in 5 seconds
   }
} // set TimefromUdp

unsigned long getFromNTP(){
  
  sendNTPpacket(timeServer);
  
  delay(1000);
  int cb = udp.parsePacket();
  if(!cb){
    return 0UL;
  }
  
  // We've received a packet, read the data from it
  udp.read(packetBuffer, NTP_PACKET_SIZE);
  //the timestamp starts at byte 40 of the received packet and is four bytes, or two words, long. First, extract the two words:
  unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
  unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
  
  return (unsigned long) highWord << 16 | lowWord;
}

int dst (time_t t) // calculate if summertime in Europe
{
   tmElements_t te;
   te.Year = year(t)-1970;
   te.Month =3;
   te.Day =1;
   te.Hour = 0;
   te.Minute = 0;
   te.Second = 0;
   time_t dstStart,dstEnd, current;
   dstStart = makeTime(te);
   dstStart = lastSunday(dstStart);
   dstStart += 2*SECS_PER_HOUR;  //2AM
   te.Month=10;
   dstEnd = makeTime(te);
   dstEnd = lastSunday(dstEnd);
   dstEnd += SECS_PER_HOUR;  //1AM
   if (t>=dstStart && t<dstEnd) return (3600);  //Add back in one hours worth of seconds - DST in effect
   else return (0);  //NonDST
}

time_t lastSunday(time_t t)
{
   t = nextSunday(t);  //Once, first Sunday
   if(day(t) < 4) return t += 4 * SECS_PER_WEEK;
   else return t += 3 * SECS_PER_WEEK;
}

//////////////////////////
//Handle webserver calls//
//////////////////////////
void handleRoot() {
  Serial.println("Handeling a call to the root");
  server.send(200, "text/plain", "hello from esp8266!");
}

void handleSetLight(){
  Serial.println("Handeling a light that's setting a light");

  if(!active()){
    String json_response = "{\"result\":\"inactive\", \"start_hour\":\"";
    json_response += timeStart;
    json_response += "\", \"end_hour\":\"";
    json_response += timeEnd;
    json_response += "\"}";
    server.send(200, "application/json", json_response);
    return;
  }
  
  for (uint8_t i=0; i<server.args(); i++){
    Serial.println( server.argName(i) + ": " + server.arg(i) );
    if(server.argName(i) == "red"){
      redValue = server.arg(i);
    }else if(server.argName(i) == "orange"){
      orangeValue = server.arg(i);
    }else if(server.argName(i) == "green"){
      greenValue = server.arg(i);
    }else if(server.argName(i) == "bus"){
      busValue = server.arg(i);
    }
  }

  server.send(200, "application/json", "{\"result\":\"handled\"}");
  
}

void handleGetLight(){
  Serial.println("Handeling getLight request");
  server.send(200, "application/json", "{\"result\":\"success\", \"settings\":{\"red\":\""+redValue+"\", \"orange\":\""+orangeValue+"\", \"green\":\""+greenValue+"\", \"bus\":\""+busValue+"\"} }");
}

void handleNotFound(){
  Serial.println("Handeling a call to NOT FOUND");
  String message = "You are makeing a non-existing call.\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  
}


///////////////////////////////////////////
//Some functions to blink lights etcetera//
///////////////////////////////////////////
void startupBlink(){
  
  digitalWrite(pinLightRed, HIGH);
  digitalWrite(pinLightOrange, HIGH);
  digitalWrite(pinLightGreen, HIGH);
  digitalWrite(pinLightBus, HIGH);

  delay(250);
  
  digitalWrite(pinLightRed, LOW);
  digitalWrite(pinLightOrange, LOW);
  digitalWrite(pinLightGreen, LOW);
  digitalWrite(pinLightBus, LOW);

  delay(250);

  digitalWrite(pinLightRed, HIGH);
  digitalWrite(pinLightOrange, HIGH);
  digitalWrite(pinLightGreen, HIGH);
  digitalWrite(pinLightBus, HIGH);

  delay(250);

  digitalWrite(pinLightRed, LOW);
  digitalWrite(pinLightOrange, LOW);
  digitalWrite(pinLightGreen, LOW);
  digitalWrite(pinLightBus, LOW);
  
}

void startupLoop(){
  
  digitalWrite(pinLightRed, HIGH);
  digitalWrite(pinLightOrange, LOW);
  digitalWrite(pinLightGreen, LOW);
  digitalWrite(pinLightBus, LOW);

  delay(700);

  digitalWrite(pinLightRed, LOW);
  digitalWrite(pinLightOrange, HIGH);
  digitalWrite(pinLightGreen, LOW);
  digitalWrite(pinLightBus, LOW);

  delay(700);

  digitalWrite(pinLightRed, LOW);
  digitalWrite(pinLightOrange, LOW);
  digitalWrite(pinLightGreen, HIGH);
  digitalWrite(pinLightBus, LOW);

  delay(700);

  digitalWrite(pinLightRed, LOW);
  digitalWrite(pinLightOrange, LOW);
  digitalWrite(pinLightGreen, LOW);
  digitalWrite(pinLightBus, HIGH);
}

