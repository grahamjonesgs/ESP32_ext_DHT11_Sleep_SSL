/* Project ESP32, DHT, MQTT and Deepsleep

  For new rooms check DHT type, topics and board type

  Check any debug chnages to sleep time
  Need library
  - PubSubCleint
  - DHT Sensor Library
  

*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "time.h"
#include "ESP32_config.h"
#include "DHT.h"


/* Configuration Section */
#define ROOM "bedroom"                      // Room for topic
#define DHTPIN 22                          // DHT Data Pin 
//define DHTTYPE DHT11                      // DHT type 11
#define DHTTYPE DHT22                      // DHT type 22
//#define LED_PIN  5                       // The builtin LED - hardcode 5 for TTGO board, make LED_BUILTIN for other boards
#define LED_PIN  LED_BUILTIN
#define BATT_PIN 0                         // Battery measurement PIN - built in on 35 on TTGO board, set to zero for no battery

bool debug_serial = true;                 // Display log message if true to serial
bool debug_mqtt = true;                   // Log to mqtt debug topic if true

#define MQTT_TOPIC_USER "gBridge/u1935/"           //MQTT topic prefic
#define MQTT_TEMP_TOPIC  "/tempset-ambient/set"
#define MQTT_HUMID_TOPIC  "/tempset-humidity/set"
#define MQTT_DEBUG_TOPIC "/debug"
/* End configuration Section */

// definitions for deepsleep
#define uS_TO_S_FACTOR 1000000LL          // Conversion factor for micro seconds to seconds (LL to force long maths to avoid overflow)
#define TIME_TO_SLEEP 30                 // Time to sleep normally for success 
#define TIME_TO_SLEEP_DHT_ERROR 30      // Time to sleep in case of DHT error (hardware error)  
#define TIME_TO_SLEEP_MQTT_ERROR 30     // Time to sleep in case of MQTT connection error
#define TIME_TO_SLEEP_WIFI_ERROR 30      // Time to sleep in case of WiFI error
#define TIME_TO_SLEEP_FIRST_WIFI_ERROR 5  // Time to sleep in case of initial WiFI error - sometime does not connect at first
#define WIFI_RETRIES 5                    // Number of times to retry the wifi before a restart
#define FIRST_WIFI 5                      // Number of boot count of first wifi, with the shorter sleep time 
#define MQTT_RETRIES 2                    // Number of times to retry the mqtt before a restart

//Variable to persist deep sleep
RTC_DATA_ATTR int bootCount = 0;          // To count the number of boot from deep sleep
RTC_DATA_ATTR int successCount = 0;       // To count the number of succesful reading sent
RTC_DATA_ATTR bool sucessFlash = false;   // To only send the success flash once per reset.

//Global variables
bool firstSerial = true;                  // To enable a return before the first message of the wakeup
float halfVoltageValue = 0.0;             // Raw read from input pin
#define RAW_VOLTS_CONVERTION 620.5        // Mapping raw input back to voltage  4096 / (3.3 * 2)
float volts = 0.0;                        // Converted to voltage (doubled and mapped back from scale of 0 - 4096 for 0 - 3.3V)
RTC_DATA_ATTR float initialVolts = 0.0;   // To count the number of boot from deep sleep
String batteryMessage;                    
String temperature_topic;
String humidity_topic;
String debug_topic;


//Definitions for NPT time
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;
struct tm timeinfo;
char timeStringBuff[50];

// Create objects
DHT dht(DHTPIN, DHTTYPE);
WiFiClientSecure espClient;
PubSubClient client(espClient);

void setup() {
  //Set up topics
  temperature_topic = String(MQTT_TOPIC_USER) + String(ROOM) + String(MQTT_TEMP_TOPIC);       // Topic temperature
  humidity_topic = String(MQTT_TOPIC_USER) + String(ROOM) + String(MQTT_HUMID_TOPIC);         // Topic humidity
  debug_topic = String(MQTT_TOPIC_USER) + String(ROOM) + String(MQTT_DEBUG_TOPIC);            // Topic Debug

  if (debug_serial) {
    Serial.begin(115200);
  }
  bootCount++;
  //Set up built in LED as message light
  pinMode(LED_PIN, OUTPUT);

  setup_wifi();                           //Connect to Wifi network


  client.setServer(MQTT_SERVER, 8883);
  if (!client.connected()) {
    reconnect();
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  if (!getLocalTime(&timeinfo)) {
    strcpy(timeStringBuff, "Time Error");
  }
  else
  {
    strftime(timeStringBuff, sizeof(timeStringBuff), "%d/%m/%y %H:%M:%S", &timeinfo);
  }


  dht.begin();
  delay(2000);
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if ( isnan(t) || isnan(h)) {
    debug_message(String(timeStringBuff) + " DHT Error", true);
    flash_led(false);  //three flash error cycles for DHT error
    delay(500);
    flash_led(false);
    delay(500);
    flash_led(false);
    deep_sleep(TIME_TO_SLEEP_DHT_ERROR);
  }

  successCount++;
  if (BATT_PIN) {
    halfVoltageValue = analogRead(BATT_PIN);
    volts = halfVoltageValue / RAW_VOLTS_CONVERTION;
    if (volts > initialVolts) {
      initialVolts = volts;
    }
    batteryMessage = " | Bat: " + String(volts, 2) + "V/" + String(initialVolts, 2) + "V";
  }
  else
  {
    batteryMessage = " | No Batt";
  }
  debug_message(String(timeStringBuff) + " | T: " + String(t, 1) + " | H: " + String(h, 0) + batteryMessage + " | Boot: " + String(bootCount) + " | Success: " + String(successCount), true);
  client.publish(temperature_topic.c_str(), String(t).c_str(), false);   // Publish temperature
  delay(100); //some delay is needed for the mqtt server to accept the message
  client.publish(humidity_topic.c_str(), String(h).c_str(), false);      // Publish humidity
  if (!sucessFlash) {
    flash_led(true);
    sucessFlash = true; //only do this once per external reset
  }
  deep_sleep(TIME_TO_SLEEP);
}

//Setup connection to wifi
void setup_wifi() {
  int counter = 1;
  delay(20);
  debug_message("Connecting to " + String(WIFI_SSID), false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    counter++;
    delay(1000);
    debug_message("Trying WiFI", false);

    if (counter > WIFI_RETRIES) {
      flash_led(false);  //one flash error cycle for wifi error
      if (bootCount < FIRST_WIFI) {  //Avoid long waits on intial errors to link to wifi.
        deep_sleep(TIME_TO_SLEEP_FIRST_WIFI_ERROR);
      }
      else
      {
        deep_sleep(TIME_TO_SLEEP_WIFI_ERROR);
      }
    }
  }

  debug_message("WiFi is OK => ESP32 new IP address is: " + WiFi.localIP().toString(), false);

}

//Reconnect to wifi if connection is lost
void reconnect() {
  int counter = 1;
  while (!client.connected()) {
    debug_message("Connecting to MQTT broker ...", false);
    if (client.connect("ESP32Client", MQTT_USER, MQTT_PASSWORD)) {
      debug_message("MQTT link OK", false);
    } else {
      debug_message("[Error] MQTT not connected: " + String(client.state()), false );
      counter++;
      if (counter > MQTT_RETRIES) {
        flash_led(false);  //two flash error cycles for mqtt error
        delay(500);
        flash_led(false);
        deep_sleep(TIME_TO_SLEEP_MQTT_ERROR);
      }
    }
  }
}

void debug_message(String message, bool perm) {
  if (debug_mqtt) {
    client.publish(debug_topic.c_str(), message.c_str(), perm);  //publish non retained mqtt message
  }
  if (debug_serial) {
    if (firstSerial) {                                     //New line of first message of wake up as there is junk in the buffer
      Serial.println("");
      firstSerial = false;
    }
    Serial.println(message);                              //send message to serial
  }
}


void flash_led(bool isok) {
  //3 slow blinks for OK, 10 fast for error
  if (!isok) {
    for (int i = 0; i < 10; i++) {        //Error case
      digitalWrite(LED_PIN, HIGH);
      delay(50);
      digitalWrite(LED_PIN, LOW);
      delay(50);
    }
  }
  else {
    for (int i = 0; i < 3; i++) {      //Success case
      digitalWrite(LED_PIN, HIGH);
      delay(500);
      digitalWrite(LED_PIN, LOW);
      delay(500);
    }
  }
}

void deep_sleep (int sleepSeconds) {
  esp_sleep_enable_timer_wakeup(sleepSeconds * uS_TO_S_FACTOR);
  debug_message("Setup ESP32 to sleep for " + String(sleepSeconds) + " Seconds", false);
  WiFi.disconnect();
  esp_deep_sleep_start();
}

void loop() {
}
