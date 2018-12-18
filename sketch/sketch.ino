/*
   I slapped a lot of code of Klusjesman, supersjimmie and OZNU together
   and reused the that I needed.

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

// This constant is used to filter out RFT device packets, only the packets with this ID are listened to.
const uint8_t RFTid[] = {0x66, 0xa9, 0x6a, 0xa5, 0xa9, 0xa9, 0x9a, 0x56}; 

// Replace with your network credentials
const char* ssid = "*** fill in your own SSID ***";
const char* password = "*********";

// Hostname
const char* accessoryName = "ESPFanController2";

/* change the value of repeater to true if repeater functionality is required */
bool repeater = false;

/* change the value of showEveryPacket to true if you want to see every received packet in the Serial Monitor. For instance to find the ID of your remote*/
bool showEveryPacket = false;

/* Led on or Off to use the builtin Led */
bool ledOn = false;

/* turn serial monitor on / off */
bool serialMon = true;

// Pins
#define ITHO_IRQ_PIN D2

// Custom statuses
#define STATE_OFF       HIGH
#define STATE_ON        LOW

// Current accessory/button status 
String currentState = "off";
int currentSpeed = 0;

// ITHO related matter
volatile bool ITHOhasPacket = false;
bool RFTrepeater = false;
IthoCommand RFTcommand[3] = {IthoLow, IthoMedium, IthoHigh};
byte RFTRSSI[3] = {0, 0, 0};
byte RFTcommandpos = 0;
IthoCommand RFTlastCommand = IthoLow;
IthoCommand RFTstate = IthoUnknown;
IthoCommand savedRFTstate = IthoUnknown;
bool RFTidChk[3] = {false, false, false};

// variables for the led flashing
volatile int LedFlashTimes = 0;
int LedState = HIGH; //means off
const int OFF = HIGH;
const int ON = LOW;
unsigned long prevMillLedSta = 0;
const unsigned long flashIntervalMillis = 20;

//variables for low speed setting timeout after number of millies power settings return to off
unsigned long prevMillStaChg = 0;
const unsigned long lowSpeedSettingTimeout = 11000;

void adjustSpeed(int value) {
  if (value < 34) {
    // set off
    setSpeedSetting("low");  
    if (serialMon) Serial.printf("Speed set to %d. Setting to low\n", value);
  } else if (value < 67) {
    // set medium
    setSpeedSetting("med");
    if (serialMon) Serial.printf("Speed set to %d. Setting to medium\n", value);
  } else if (value < 101) {
    // set high
    setSpeedSetting("high");
    if (serialMon) Serial.printf("Speed set to %d. Setting to high\n", value);
  } else {
    // invalid value - turn off
    togglePower(false);
    if (serialMon) Serial.printf("Speed set to %d. Invalid Speed. Turning off.\n", value);
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
  if (ledOn) LedFlashTimes += times;
}

void ITHOcheck() {
  flashLed(1);
  if (rf.checkForNewPacket()) {
    IthoCommand cmd = rf.getLastCommand();
    if (++RFTcommandpos > 2) RFTcommandpos = 0;  // store information in next entry of ringbuffers
    RFTcommand[RFTcommandpos] = cmd;
    RFTRSSI[RFTcommandpos]    = rf.ReadRSSI();
    bool chk;
    //if (showEveryPacket) chk = true;
    //else chk = rf.checkID(RFTid);
    chk = rf.checkID(RFTid);
    RFTidChk[RFTcommandpos]   = chk;
    if (((cmd != IthoUnknown) && chk) || showEveryPacket) {  // only act on good cmd and correct RFTid.
      ITHOhasPacket = true;
    }
  }
}

void ITHOinterrupt() {
  ITHOticker.once_ms(10, ITHOcheck);
}

void led() {
  unsigned long currentMillis = millis();
  if ((LedFlashTimes == 0) && (LedState == ON) && ((currentMillis - prevMillLedSta) > flashIntervalMillis)) {
    prevMillLedSta = currentMillis;
    LedState = OFF;
  } else if ((LedFlashTimes > 0) && (LedState == OFF) && ((currentMillis - prevMillLedSta) > flashIntervalMillis)) {
    LedFlashTimes--;
    prevMillLedSta = currentMillis;
    LedState = ON;
  } else if ((LedFlashTimes > 0) && (LedState == ON) && ((currentMillis - prevMillLedSta) > flashIntervalMillis)) {
    prevMillLedSta = currentMillis;
    LedState = OFF;
  } 
  digitalWrite(LED_BUILTIN, LedState);
}

void staChgTimeout() {
  unsigned long currentMillis = millis();
  if ((prevMillStaChg == 0) && (currentState == "low")) prevMillStaChg = millis();
  else if ((currentMillis - prevMillStaChg) > lowSpeedSettingTimeout) {
    if ((currentState == "low") && (currentSpeed < 34)) {
      currentSpeed = 0;
      currentState = "off";
      prevMillStaChg = 0;
      sendUpdate();
    } else prevMillStaChg = 0;   
  }
}

void repeatReceivedPacketCommand() {
  ITHOhasPacket = false;
  uint8_t goodpos = findRFTlastCommand();
  if (goodpos != -1)  RFTlastCommand = RFTcommand[goodpos];
  else                RFTlastCommand = IthoUnknown;
  if (serialMon) Serial.print("Repeating command: [");
  if (serialMon) Serial.print(RFTlastCommand);
  rf.sendCommand(RFTlastCommand);
  if (serialMon) Serial.println("]\n");
}

void sendHighSpeed() {
  if (serialMon) Serial.println("sending high via RF...");
  rf.sendCommand(IthoHigh);
  if (serialMon) Serial.println("sending high done.");
}

void sendLowSpeed() {
  if (serialMon) Serial.println("sending low via RF ...");
  rf.sendCommand(IthoLow);
  if (serialMon) Serial.println("sending low done.");
}

void sendMediumSpeed() {
  if (serialMon) Serial.println("sending medium via RF...");
  rf.sendCommand(IthoMedium);
  if (serialMon) Serial.println("sending medium done.");
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
  if (serialMon) Serial.println("currentState is updated at HomeBridge.\n");
  //flashLed(2);
}

void setSpeedSetting(String setting) {
  if (setting != currentState) {
    currentState = setting;
    if (setting == "low") sendLowSpeed();
    else if (setting == "med") sendMediumSpeed();
    else if (setting == "high") sendHighSpeed();
    sendUpdate();
  }
}

void updateCurrentStateFromRFCommand() {
  ITHOhasPacket = false;
  uint8_t goodpos = findRFTlastCommand();
  if (goodpos != -1)  RFTlastCommand = RFTcommand[goodpos];
  else                RFTlastCommand = IthoUnknown;
  switch (RFTlastCommand) {
    case IthoLow:
      currentState = "low";
      currentSpeed = 0;
      sendUpdate();
      break;
    case IthoMedium:
      currentState = "medium";
      currentSpeed = 50;
      sendUpdate();
      break;
    case IthoHigh:
      currentState = "high";
      currentSpeed = 100;
      sendUpdate();
      break;
  }
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
  Serial.print("##### End of packet #####\n");
}

void togglePower(bool value) {
  if (value && (currentSpeed == 0)) {
    currentSpeed = 100;
    adjustSpeed(currentSpeed);
  }
  else if (value) adjustSpeed(currentSpeed);
  else {
    if (serialMon) Serial.println("Toggle Power to low\r\n");
    currentSpeed = 0;
    setSpeedSetting("low");
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      if (serialMon) Serial.printf("[%u] Disconnected!\r\n", num);
      break;
    case WStype_CONNECTED:
      if (serialMon) Serial.printf("[%u] Connected from url: %s\r\n", num, payload);
      sendUpdate();
      break;
    case WStype_TEXT: {
      if (serialMon) Serial.printf("[%u] get Text: %s\r\n", num, payload);

      DynamicJsonBuffer jsonBuffer;
      JsonObject& root = jsonBuffer.parseObject((char *)&payload[0]);

      if (root.containsKey("speed") && currentState != "off") currentSpeed = root["speed"];
      if (root.containsKey("power")) togglePower(root["power"]);
      else if (root.containsKey("speed")) {
        currentSpeed = root["speed"];
        adjustSpeed(root["speed"]);
      }
      break;
    }
    case WStype_BIN:
      if (serialMon) Serial.printf("[%u] get binary length: %u\r\n", num, length);
      break;
    default:
      if (serialMon) Serial.printf("Invalid WStype [%d]\r\n", type);
      break;
  }
}

/* Setup function region
 * For readability I created function for setup tasks
 * I could normalize these out, no time for that at this moment.
 */

void setupLED() {
    pinMode(LED_BUILTIN, OUTPUT);
}

void setupMDNS() {
  if (mdns.begin(accessoryName, WiFi.localIP())) {
    if (serialMon) Serial.println("MDNS responder started");
  }
}

void setupRF() {
  rf.init();
  delay(100);
  rf.initReceive();
  delay(100);
  pinMode(ITHO_IRQ_PIN, INPUT_PULLUP);
  attachInterrupt(ITHO_IRQ_PIN, ITHOinterrupt, CHANGE);
  //rf.sendCommand(IthoJoin); //the ID inside ithoCC1101.cpp at this->outIthoPacket.deviceId2 is used!
  //rf.sendCommand(IthoLeave); //the ID inside ithoCC1101.cpp at this->outIthoPacket.deviceId2 is used!
  delay(100);
}

void setupSocketServer() {
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  if (serialMon) Serial.println("Web socket server started on port 81");
}

void setupWiFi(){
  // connect to wifi
  WiFi.mode(WIFI_STA);
  WiFi.hostname(accessoryName);
  WiFi.begin(ssid, password);
  if (serialMon) Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    if (serialMon) Serial.print(".");
    delay(500);
  }
  if (serialMon) Serial.println("");
  if (serialMon) Serial.print("Connected to ");
  if (serialMon) Serial.println(ssid);
  if (serialMon) Serial.print("IP address: ");
  if (serialMon) Serial.println(WiFi.localIP());
}

void setup(void) {
  if (serialMon) Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  if (serialMon) Serial.println("\n######  begin setup  ######");
  if (ledOn) setupLED();
  setupRF();
  setupWiFi();
  setupMDNS();
  setupSocketServer();
  if (serialMon) Serial.println("######  setup done   ######\n");
}

void loop(void) {
  if (ledOn) led();
  webSocket.loop();
  if (ITHOhasPacket) { // ITHOhasPacket is only true if the packet was send from the RFT with corresponding RTFid
    updateCurrentStateFromRFCommand();
    if (serialMon) showPacket();
    if(repeater) repeatReceivedPacketCommand();
  }
  staChgTimeout();
  yield();
}
