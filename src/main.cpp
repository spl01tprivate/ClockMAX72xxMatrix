// ***** INCLUDES *****
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <MD_MAX72xx.h>
#include <String>
#include <EEPROM.h>

// ***** DEFINES *****
#define wifi_ssid "SploitNetwork"
#define wifi_password "19_bennO_network"

#define mqtt_server "192.168.0.2"
#define mqtt_user "Sploit"
#define mqtt_password "hassio_home"

// TX
#define status_topic "d1m_003/status"
#define lightstate_tx_topic "d1m_003/lightstatetx"

// RX
#define intensity_rx_topic "d1m_003/intensity"
#define daylightsaving_rx_topic "d1m_003/daylightsaving"
#define lightstate_rx_topic "d1m_003/lightstaterx"
#define power_rx_topic "d1m_003/power"

// GPIOs
#define touchPin 12 // D6

// ***** VARIABLES *****
const long utcOffsetInSeconds = 7200;
char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
bool firstRead = true;
int lastReadMinute = -1;
int lastReadSecond = -1;
String minutesBefore;
int hour; // For Daylight Saving option
bool daylightsaving = false;
String clientId = "d1m_003-" + String(random(0xffff), HEX);
unsigned long lastMQTTTry = 0;
unsigned long lastWiFiTry = 0;
bool conTryActive = false;
bool wifiReady = false;
bool firstMQTTCon = true;
int matPtrMQTT = 0;
int matPtrWIFI = 31;
unsigned long lastMatMQTTMove = 0;
unsigned long lastMatWIFIMove = 0;
bool displayWIFIdsctnd = false;
bool touchInput = false;
bool contentTimeDate = true;
bool lightStateMQTT = false;
bool powerState = true;
unsigned int lm_intensity = 0;

// ***** OBJECTS *****
// Network
WiFiClient espClient;
PubSubClient client(espClient);
WiFiEventHandler onStationModeConnectedHandler;

// Network Time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", utcOffsetInSeconds);

// LED Matrix
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4

#define CLK_PIN 14  // or SCK D5
#define DATA_PIN 13 // or MOSI D7
#define CS_PIN 15   // or SS D8

MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

void displayText(String text)
{
  mx.clear();

  for (unsigned int i = 0; i < text.length(); i++)
  {
    mx.setChar((i * COL_SIZE) + COL_SIZE - 3, text[3 - i]);
  }
}

void intensity(unsigned int val)
{
  mx.control(MD_MAX72XX::INTENSITY, val);
}

// ***** PROTOTYPES *****
void initServices();
void setup_wifi();
void callback(char *, byte *, unsigned int);
void checkMQTT();
void printTime(bool);
void updateHandler();
void signalConnectionsMatrix();
void readInput();

void onStationConnected(const WiFiEventStationModeConnected &evt)
{
  Serial.print("\n[WiFi] Successfully connected to '" + String(evt.ssid) + "' with IP '");
  IPAddress myIP = WiFi.localIP();
  Serial.println(myIP);
  wifiReady = true;
  if (matPtrWIFI == 31)
    matPtrWIFI = 24;
  else
    matPtrWIFI++;
  mx.setPoint(7, matPtrWIFI, false);
  initServices();
  lastReadSecond = timeClient.getSeconds();
  String text = "MQTT";
  if (!client.connected())
  {
    for (unsigned int i = 0; i < text.length(); i++)
    {
      mx.setChar((i * COL_SIZE) + COL_SIZE - 3, text[3 - i]);
    }
  }
}

// ***** SETUP *****
void setup()
{
  Serial.begin(115200);
  while (!Serial)
    ;

  // EEPROM
  EEPROM.begin(1);
  if (EEPROM.read(0) == 1)
    daylightsaving = true;
  else
    daylightsaving = false;

  // Display
  mx.begin();
  mx.clear();
  intensity(lm_intensity);
  // mx.transform(MD_MAX72XX::TINV);

  // Networking
  onStationModeConnectedHandler = WiFi.onStationModeConnected(&onStationConnected);
  setup_wifi();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  checkMQTT();
}

// ***** LOOP *****
void loop()
{
  readInput();

  printTime(false);

  signalConnectionsMatrix();

  updateHandler();
}

// ***** FUNCTIONS *****
void initServices()
{
  // NTP - Network Time
  timeClient.begin();

  // OTA-Init
  ArduinoOTA.setHostname("d1m_003_ledmatrix");
  ArduinoOTA.setPassword("hassio_home");
  ArduinoOTA.begin();

  //*****Other*****
  timeClient.update(); // NTP
  client.loop();       // MQTT Message Handler
  ArduinoOTA.handle(); // OTA Update Handler
}

void updateHandler()
{
  if (!client.connected() && wifiReady)
    checkMQTT();
  else
    client.loop(); // MQTT Message Handler

  if (WiFi.status() != WL_CONNECTED)
    setup_wifi();

  if (wifiReady)
  {
    timeClient.update(); // NTP
    ArduinoOTA.handle(); // OTA Update Handler
  }

  if (firstRead)
    firstRead = false;

  yield();
}

void readInput()
{
  if (digitalRead(touchPin) && !touchInput)
  {
    touchInput = true;
    Serial.println("\n[INPUT] Touch detected!");
    contentTimeDate = false;
    if (lightStateMQTT)
      displayText("Off ");
    else
      displayText(" On ");
    client.publish(lightstate_tx_topic, lightStateMQTT ? "0" : "1");
  }
  if (!digitalRead(touchPin) && touchInput)
  {
    touchInput = false;
    Serial.println("\n[INPUT] Touch released!");
  }
}

void printTime(bool forceDraw)
{
  if ((((forceDraw || firstRead || timeClient.getSeconds() == lastReadSecond) && WiFi.status() == WL_CONNECTED && contentTimeDate) || (!contentTimeDate && !touchInput)) && powerState)
  {
    lastReadSecond = timeClient.getSeconds();
    if (lastReadSecond == 59)
      lastReadSecond = 0;
    else
      lastReadSecond++;

    Serial.println();
    Serial.print(daysOfTheWeek[timeClient.getDay()]);
    Serial.print(", ");
    hour = timeClient.getHours();
    if (daylightsaving)
    {
      if (hour == 0)
        hour = 23;
      else
        hour--;
    }
    Serial.print(hour);
    Serial.print(":");
    Serial.print(timeClient.getMinutes());
    Serial.print(":");
    Serial.println(timeClient.getSeconds());
    Serial.println();

    if ((forceDraw || firstRead || minutesBefore != String(timeClient.getMinutes())) || (!contentTimeDate && !touchInput))
    {
      contentTimeDate = true;

      minutesBefore = String(timeClient.getMinutes());

      String hours;
      String minutes;

      if (hour < 10)
      {
        hours = "0" + String(hour);
      }
      else
      {
        hours = String(hour);
      }

      if (timeClient.getMinutes() < 10)
      {
        minutes = "0" + String(timeClient.getMinutes());
      }
      else
      {
        minutes = String(timeClient.getMinutes());
      }

      displayText(hours + minutes);
      displayWIFIdsctnd = false;
    }
  }
  else if ((WiFi.status() == WL_CONNECTED && contentTimeDate && !powerState) || (WiFi.status() == WL_CONNECTED && !digitalRead(touchPin) && !powerState))
  {
    intensity(0);
    mx.clear();
  }
}

void signalConnectionsMatrix()
{
  if (!client.connected() && millis() > (lastMatMQTTMove + 500))
  {
    lastMatMQTTMove = millis();
    if (matPtrMQTT == 8)
      matPtrMQTT = 0;
    mx.setPoint(7, matPtrMQTT, true);
    if (matPtrMQTT == 0)
      matPtrMQTT = 7;
    else
      matPtrMQTT--;
    mx.setPoint(7, matPtrMQTT, false);
    if (matPtrMQTT == 7)
      matPtrMQTT = 1;
    else
      matPtrMQTT += 2;
  }
  if (!wifiReady && millis() > (lastMatWIFIMove + 500))
  {
    lastMatWIFIMove = millis();
    if (matPtrWIFI == 23)
      matPtrWIFI = 31;
    mx.setPoint(7, matPtrWIFI, true);
    if (matPtrWIFI == 31)
      matPtrWIFI = 24;
    else
      matPtrWIFI++;
    mx.setPoint(7, matPtrWIFI, false);
    if (matPtrWIFI == 24)
      matPtrWIFI = 30;
    else
      matPtrWIFI -= 2;
  }
  if (!wifiReady && !displayWIFIdsctnd)
  {
    displayWIFIdsctnd = true;
    String text = "WIFI";
    for (unsigned int i = 0; i < text.length(); i++)
    {
      mx.setChar((i * COL_SIZE) + COL_SIZE - 3, text[3 - i]);
    }
  }
}

void callback(char *topic, byte *message, unsigned int length)
{
  Serial.print("\n[MQTT] Message arrived on topic '");
  Serial.print(topic);
  Serial.print("' - Data '");
  String messageTemp;

  // Convert and print Message
  for (unsigned int i = 0; i < length; i++)
  {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println("'");

  // Check whether Message is subscribed
  if (String(topic) == intensity_rx_topic) // subscribed topic defined in header ! Topic subscription is done in the reconnect() Function below !
  {
    lm_intensity = atoi(messageTemp.c_str());
    intensity(lm_intensity);
    Serial.println("\n[MQTT] Intensity was set to " + String(messageTemp) + "!");
  }
  else if (String(topic) == daylightsaving_rx_topic) // subscribed topic defined in header ! Topic subscription is done in the reconnect() Function below !
  {
    if (messageTemp == "0" || messageTemp == "off" || messageTemp == "false")
    {
      daylightsaving = false;
      Serial.println("\n[MQTT] DLS was turned off!");
    }
    else
    {
      daylightsaving = true;
      Serial.println("\n[MQTT] DLS was turned on!");
    }
    EEPROM.write(0, daylightsaving ? 1 : 0);
    EEPROM.commit();
    printTime(true);
  }
  else if (String(topic) == lightstate_rx_topic)
  {
    if (messageTemp == "off")
      lightStateMQTT = false;
    else if (messageTemp == "on")
      lightStateMQTT = true;

    Serial.println("\n[MQTT] Light state: " + String(messageTemp));
  }
  else if (String(topic) == power_rx_topic)
  {
    if (messageTemp == "off")
    {
      powerState = false;
    }
    else if (messageTemp == "on")
    {
      powerState = true;
      intensity(lm_intensity);
      printTime(true);
    }
    Serial.println("\n[MQTT] Power State: " + String(messageTemp));
  }
  else
  {
    Serial.println("[MQTT] Not a subscribed topic (0x1)");
  }
}

void setup_wifi()
{
  if (((WiFi.status() != WL_CONNECTED) && millis() > (lastWiFiTry + 10000)) || firstRead)
  {
    wifiReady = false;
    lastWiFiTry = millis();
    Serial.println("\n[WiFi] Connecting to " + String(wifi_ssid));
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_password);
  }
}

void checkMQTT()
{
  if (((!client.connected() && millis() > (lastMQTTTry + 10000)) || firstMQTTCon) && wifiReady)
  {
    if (firstMQTTCon)
      firstMQTTCon = false;

    lastMQTTTry = millis();

    if (!conTryActive)
    {
      conTryActive = true;
      Serial.println("\n[MQTT] Attempting MQTT connection...");
      client.connect(clientId.c_str(), mqtt_user, mqtt_password);
    }
    else
    {
      Serial.println("\n[MQTT] Connection failed within 10 seconds!");
      Serial.println("\n[MQTT] Attempting another MQTT connection...");
      client.connect(clientId.c_str(), mqtt_user, mqtt_password);
    }
  }
  if (client.connected() && conTryActive)
  {
    conTryActive = false;
    Serial.println("\n[MQTT] Succesfully connected to MQTT-Server with following IP: " + String(mqtt_server));
    client.subscribe(intensity_rx_topic);
    client.subscribe(daylightsaving_rx_topic);
    client.subscribe(lightstate_rx_topic);
    client.subscribe(power_rx_topic);
    client.publish(status_topic, "d1m_003 active as ledmatrix");
    if (matPtrMQTT == 0)
      matPtrMQTT = 7;
    else
      matPtrMQTT--;
    mx.setPoint(7, matPtrMQTT, false);
  }
}