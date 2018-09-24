/*
   I slapped a lot of code of Klusjesman, supersjimmie and OZNU together
   and reused the best pieces.

   This sketch is build to create a standard interface exposed on local network
   and can utilize a CC2201 to send RF signals to an Itho ECO CVE

   This is inspired by the little project of adri phillips.

  Current features:
  * Connects to WiFi netwerk as a station using ESP8266WiFi 
  * Prints the received IP address to serial at 115200 baud
  * Provides a websocketServer using websockets
  * Responds to rft packets picked up with the CC2201, if the RFTid is the same as configured under RFTid[] the desired state is saved and printed to serial.
  * Responds to commands from the homebridge plugin: https://github.com/oznu/homebridge-esp8266-fan
  * This solution is also usable as a repeater, which results in an increased signal range of the RFT.
*/

#include <ArduinoJson.h>
#include <SPI.h>
#include "IthoCC1101.h"
#include "IthoPacket.h"
#include <Ticker.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WebSocketsServer.h>
#include <ESP8266mDNS.h>

MDNSResponder mdns;
IthoCC1101 rf;
IthoPacket packet;
Ticker ITHOticker;
WebSocketsServer webSocket = WebSocketsServer(81);

// This constant is used to filter out other RFT device packets and to send packets with
// Use Serial.println("ID of sender: " + rf.getLastIDstr()); to find the sender ID of your RFT device
const uint8_t RFTid[] = {0x66, 0xa9, 0x6a, 0xa5, 0xa9, 0xa9, 0x9a, 0x56}; 

// Replace with your network credentials
const char* ssid = "***************";
const char* password = "***************";

// Hostname
const char* accessoryName = "ESPFanController";

// Pins
const int POWER_PIN = 2;
const int SPEED_LOW_PIN = 0;
const int SPEED_MED_PIN = 4;
const int SPEED_MAX_PIN = 5;

#define STATE_OFF       HIGH
#define STATE_ON        LOW
#define ITHO_IRQ_PIN D2

String currentState = "off";
int currentSpeed = 0;

/* change the value of repeater to true if repeater functionality is required, 
 * you can also change this value by navigating to http://xxx.xxx.xxx.xxx/repeater?value=true */
bool repeater = false;

// ITHO related matter
bool ITHOhasPacket = false;
bool RFTrepeater = false;
IthoCommand RFTcommand[3] = {IthoLow, IthoMedium, IthoHigh};
byte RFTRSSI[3] = {0, 0, 0};
byte RFTcommandpos = 0;
IthoCommand RFTlastCommand = IthoLow;
IthoCommand RFTstate = IthoUnknown;
IthoCommand savedRFTstate = IthoUnknown;
bool RFTidChk[3] = {false, false, false};

// variables for the led flashing
int LedFlashTimes = 0;
int LedState = HIGH; //means off
const int OFF = HIGH;
const int ON = LOW;
unsigned long previousMillisLedStateChange = 0;
const unsigned long flashIntervalMillis = 20;

void adjustSpeed(int value) {
  if (value < 34) {
    // set low
    setSpeedSetting("low");
    
    Serial.printf("Speed set to %d. Setting to low\n", value);
  } else if (value < 67) {
    // set medium
    setSpeedSetting("med");
    Serial.printf("Speed set to %d. Setting to medium\n", value);
  } else if (value < 101) {
    // set high
    setSpeedSetting("max");
    Serial.printf("Speed set to %d. Setting to high\n", value);
  } else {
    // invalid value - turn off
    togglePower(false);
    Serial.printf("Speed set to %d. Invalid Speed. Turning off.\n", value);
  }
}

uint8_t findRFTlastCommand() {
  if (RFTcommand[RFTcommandpos] != IthoUnknown)               return RFTcommandpos;
  if ((RFTcommandpos == 0) && (RFTcommand[2] != IthoUnknown)) return 2;
  if ((RFTcommandpos == 0) && (RFTcommand[1] != IthoUnknown)) return 1;
  if ((RFTcommandpos == 1) && (RFTcommand[0] != IthoUnknown)) return 0;
  if ((RFTcommandpos == 1) && (RFTcommand[2] != IthoUnknown)) return 2;
  if ((RFTcommandpos == 2) && (RFTcommand[1] != IthoUnknown)) return 1;
  if ((RFTcommandpos == 2) && (RFTcommand[0] != IthoUnknown)) return 0;
  return -1;
}

void flashLed(int times){
  LedFlashTimes += times;
}

void ITHOcheck() {
  flashLed(1);
  if (rf.checkForNewPacket()) {
    IthoCommand cmd = rf.getLastCommand();
    if (++RFTcommandpos > 2) RFTcommandpos = 0;  // store information in next entry of ringbuffers
    RFTcommand[RFTcommandpos] = cmd;
    RFTRSSI[RFTcommandpos]    = rf.ReadRSSI();
    bool chk = rf.checkID(RFTid);
    RFTidChk[RFTcommandpos]   = chk;
    if ((cmd != IthoUnknown) && chk) {  // only act on good cmd and correct RFTid.
      ITHOhasPacket = true;
    }
  }
}

void ITHOinterrupt() {
  ITHOticker.once_ms(10, ITHOcheck);
}

void ledState() {
  unsigned long currentMillis = millis();
  if ((LedFlashTimes == 0) && (LedState == ON) && ((currentMillis - previousMillisLedStateChange) > flashIntervalMillis)) {
    previousMillisLedStateChange = currentMillis;
    LedState = OFF;
  } else if ((LedFlashTimes > 0) && (LedState == OFF) && ((currentMillis - previousMillisLedStateChange) > flashIntervalMillis)) {
    LedFlashTimes--;
    previousMillisLedStateChange = currentMillis;
    LedState = ON;
  } 
  else if ((LedFlashTimes > 0) && (LedState == ON) && ((currentMillis - previousMillisLedStateChange) > flashIntervalMillis)) {
    previousMillisLedStateChange = currentMillis;
    LedState = OFF;
  } 
  digitalWrite(LED_BUILTIN, LedState);
}

void repeatReceivedPacketCommand() {
  ITHOhasPacket = false;
  uint8_t goodpos = findRFTlastCommand();
  if (goodpos != -1)  RFTlastCommand = RFTcommand[goodpos];
  else                RFTlastCommand = IthoUnknown;
  Serial.print("Repeating command: [");
  Serial.print(RFTlastCommand);
  rf.sendCommand(RFTlastCommand);
  Serial.println("]\n");
}

void sendHighSpeed() {
  Serial.println("sending high via RF...");
  rf.sendCommand(IthoHigh);
  Serial.println("sending high done.");
}

void sendLowSpeed() {
  Serial.println("sending low via RF ...");
  rf.sendCommand(IthoLow);
  Serial.println("sending low done.");
}

void sendMediumSpeed() {
  Serial.println("sending medium via RF...");
  rf.sendCommand(IthoMedium);
  Serial.println("sending medium done.");
}

void sendUpdate() {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();

  root["speed"] = currentSpeed;

  if (currentState != "off") { // if (digitalRead(POWER_PIN) == STATE_ON) { //De status blijft nu gelijk
    root["power"] = true;
  } else {
    root["power"] = false;
  }

  String res;
  root.printTo(res);

  webSocket.broadcastTXT(res);
  flashLed(2);
}

void setSpeedSetting(String setting) {
  if (setting != currentState) {
    currentState = setting;

    if (setting == "low"|| setting == "off") sendLowSpeed();
    else if (setting == "med") sendMediumSpeed();
    else if (setting == "max" || setting == "high") sendHighSpeed();
  }
  sendUpdate();
}

void updateCurrentStateFromRFCommand() {
  ITHOhasPacket = false;
  uint8_t goodpos = findRFTlastCommand();
  if (goodpos != -1)  RFTlastCommand = RFTcommand[goodpos];
  else                RFTlastCommand = IthoUnknown;
  switch (RFTlastCommand) {
    case IthoLow:
      currentState = "off";
      currentSpeed = 0;
      break;
    case IthoMedium:
      currentState = "medium";
      currentSpeed = 50;
      break;
    case IthoHigh:
      currentState = "high";
      currentSpeed = 100;
      break;
  }
  Serial.print("currentState is now updated to : %s and currentSpeed is %u\r\n");
  sendUpdate();
  Serial.print("currentState is updated at HomeBridge.");
}

void showPacket() {
  ITHOhasPacket = false;
  uint8_t goodpos = findRFTlastCommand();
  if (goodpos != -1)  RFTlastCommand = RFTcommand[goodpos];
  else                RFTlastCommand = IthoUnknown;
  //show data
  Serial.print(F("RFT Current Pos: "));
  Serial.print(RFTcommandpos);
  Serial.print(F(", Good Pos: "));
  Serial.println(goodpos);
  Serial.print(F("Stored 3 commands: "));
  Serial.print(RFTcommand[0]);
  Serial.print(F(" "));
  Serial.print(RFTcommand[1]);
  Serial.print(F(" "));
  Serial.print(RFTcommand[2]);
  Serial.print(F(" / Stored 3 RSSI's:     "));
  Serial.print(RFTRSSI[0]);
  Serial.print(F(" "));
  Serial.print(RFTRSSI[1]);
  Serial.print(F(" "));
  Serial.print(RFTRSSI[2]);
  Serial.print(F(" / Stored 3 ID checks: "));
  Serial.print(RFTidChk[0]);
  Serial.print(F(" "));
  Serial.print(RFTidChk[1]);
  Serial.print(F(" "));
  Serial.print(RFTidChk[2]);
  Serial.print(F(" / Last ID: "));
  Serial.print(rf.getLastIDstr());

  Serial.print(F(" / Command = "));
  //show command
  switch (RFTlastCommand) {
    case IthoUnknown:
      Serial.print("unknown\n");
      break;
    case IthoLow:
      Serial.print("low\n");
      break;
    case IthoMedium:
      Serial.print("medium\n");
      break;
    case IthoHigh:
      Serial.print("high\n");
      break;
    case IthoFull:
      Serial.print("full\n");
      break;
    case IthoTimer1:
      Serial.print("timer1\n");
      break;
    case IthoTimer2:
      Serial.print("timer2\n");
      break;
    case IthoTimer3:
      Serial.print("timer3\n");
      break;
    case IthoJoin:
      Serial.print("join\n");
      break;
    case IthoLeave:
      Serial.print("leave\n");
      break;
  }
}

void togglePower(bool value) {
  if (value)
      adjustSpeed(currentSpeed);
  else {
    currentSpeed = 100;
    setSpeedSetting("off");
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\r\n", num);
      break;
    case WStype_CONNECTED:
      Serial.printf("[%u] Connected from url: %s\r\n", num, payload);
      sendUpdate();
      break;
    case WStype_TEXT: {
      Serial.printf("[%u] get Text: %s\r\n", num, payload);

      DynamicJsonBuffer jsonBuffer;
      JsonObject& root = jsonBuffer.parseObject((char *)&payload[0]);

      if (root.containsKey("power")) togglePower(root["power"]);
      else if (root.containsKey("speed")) {
        currentSpeed = root["speed"];
        adjustSpeed(root["speed"]);
      }

      break;
    }
    case WStype_BIN:
      Serial.printf("[%u] get binary length: %u\r\n", num, length);
      break;
    default:
      Serial.printf("Invalid WStype [%d]\r\n", type);
      break;
  }
}

/* Setup function region
 * For readability I created function for setup tasks
 * I could normalize these out, no time for that at this moment.
 */

void setupLED() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LedState);
}

void setupMDNS() {
  if (mdns.begin(accessoryName, WiFi.localIP())) {
    Serial.println("MDNS responder started");
  }
}

void setupRF() {
  rf.init();
  rf.initReceive();
  pinMode(ITHO_IRQ_PIN, INPUT);
  attachInterrupt(ITHO_IRQ_PIN, ITHOinterrupt, RISING);
}

void setupSocketServer() {
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("Web socket server started on port 81");
}

void setupWiFi(){
  // connect to wifi
  WiFi.mode(WIFI_STA);
  WiFi.hostname(accessoryName);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void setup(void) {
  delay(1000);
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  Serial.println();
  delay(500);
  Serial.println("######  begin setup  ######");
  setupLED();
  setupRF();
  setupWiFi();
  setupMDNS();
  setupSocketServer();
  Serial.println("######  setup done   ######");
  delay(500);
}

void loop(void) {
  ledState();
  webSocket.loop();
  if (ITHOhasPacket) { // ITHOhasPacket is only true if the packet was send from the RFT with corresponding RTFid
    updateCurrentStateFromRFCommand();
    showPacket();
    if(repeater) repeatReceivedPacketCommand();
  }
  yield();
}
