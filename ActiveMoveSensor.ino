#include <WiFi.h>
#include <Arduino.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "driver/rtc_io.h"
#include <Preferences.h>


#define TIME_TO_SLEEP  60 * 1000000ULL /* Time ESP32 will go to sleep (in seconds) */
#define SENSORS_COUNT  2
#define NOT_READY      0

const String sensor1 = "FurtkaKontraktron";
const String sensor2 = "TempZew";

typedef enum ModeType
{
    ASYNC_MOVE_SENSOR_EN,
    ASYNC_TEMP_SENSOR_EN,
    DEEP_SLEEP_EN,
    MODE_MAX
} ModeType;

// GPIO move sensor
const gpio_num_t sensorPin = GPIO_NUM_15;
// GPIO where the DS18B20 is connected to
const int oneWireBus = 4;
// GPIO connected led
const gpio_num_t connectionPin = GPIO_NUM_2;

const char* ssid = "DomWiFiExt";
const char* password = "";

String senssorConfigUrl[SENSORS_COUNT] = { "/command=GetConfigByName&sensorName="+sensor1, 
                                           "/command=GetConfigByName&sensorName="+sensor2
                                         };
                              
String sensorOnUrl  = "/command=UpdateSensorState&reading=1&sensorName="+sensor1;
String sensorOffUrl = "/command=UpdateSensorState&reading=0&sensorName="+sensor1;
String tempUrl      = "/command=UpdateSensorState&sensorName="+sensor2+"&reading=";

String findAlarmUrl = "/command=GetVersion";

hw_timer_t *timer = NULL;

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int mode = -1;
RTC_DATA_ATTR char serverIp[50] = {0};

int moveSensorState = 0;
int tempSensorState = 0;

// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(oneWireBus);
// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature sensors(&oneWire);

Preferences preferences;

void updateUrls() {

  for (int i=0;i<SENSORS_COUNT;i++) {
    senssorConfigUrl[i] = String(serverIp) + senssorConfigUrl[i];
  }
  sensorOnUrl  = String(serverIp) + sensorOnUrl;
  sensorOffUrl = String(serverIp) + sensorOffUrl;
  tempUrl      = String(serverIp) + tempUrl;
  
}

void task_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:     Serial.println("Wakeup caused by external signal using RTC_IO"); moveSensorState = 1; break;
    case ESP_SLEEP_WAKEUP_EXT1:     Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER:    Serial.println("Wakeup caused by timer");  tempSensorState = 1; break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP:      Serial.println("Wakeup caused by ULP program"); break;
    default:                        Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason); break;
  }
}

void ARDUINO_ISR_ATTR moveSensorInterrupt() {
    if (moveSensorState == 0) {
      Serial.println("----------------------------");
      Serial.println("Interrupt - sensor activated");
      moveSensorState = 1;
    }
}

// function call by the timer interruption
void ARDUINO_ISR_ATTR tempTimerInterrupt() {
    Serial.println("----------------------------");
    Serial.println("Interrupt - timer activated");  
    tempSensorState = 1;
}

void connectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(connectionPin, LOW);
    
    Serial.print("Connecting to ");
    Serial.println(ssid);

    WiFi.begin(ssid, password);
    while(WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print("-");      
    }
    
    Serial.println("-");
    Serial.print("Connected to WiFi network with IP Address: ");
    Serial.println(WiFi.localIP());
  }  
}

String sendHTTPReq(String req) {
  HTTPClient http;

  //Serial.println(req);
  connectWiFi();
  
  http.begin(req.c_str());
  http.setConnectTimeout(1000);
  //http.setTimeout(50);

  // Send HTTP GET request
  int httpResponseCode = http.GET();
  //Serial.print("Rsponse code : ");
  //Serial.println(httpResponseCode);
  String payload = http.getString();
  //Serial.println(payload);

  http.end();
  
  return (httpResponseCode == 200) ? payload : "";
}


int findServer() {
  HTTPClient http;
    
  connectWiFi();

  String ip =  WiFi.localIP().toString();
  int ip_value = 255;
  int idx = ip.lastIndexOf(".")+1;
  String result;
  String url;
  
  ip = ip.substring(0, idx);

  Serial.println("Looking for alarm system");
  
  ip_value = preferences.getUChar("ip", 0);
  if (ip_value == 255) ip_value=0;
    
  for (int i=ip_value;i<255;i++) {
    url = "http://"+ip+String(i)+findAlarmUrl;
    Serial.println(url);
    result = sendHTTPReq(url);
    if ((result.length() > 0) && (result.indexOf("<system>Alarm</system>") != -1)) {
      strcpy(serverIp, "http://");
      strcat(serverIp, ip.c_str());
      strcat(serverIp, String(i).c_str());
      digitalWrite(connectionPin, HIGH);
      preferences.putUChar("ip", (uint8_t)i);
      return 0;
    }
    else if (i == ip_value && ip_value != 0) {
      // old alarm ip is invalid - clear value in settings
      preferences.putUChar("ip", 255);
    }
  }

  Serial.println("Alarm system has not found");
  return 1;
}

void getConfiguration() {
  HTTPClient http;
  String result;
  
  connectWiFi();

  mode = 0;
  
  for (int i = 0; i < SENSORS_COUNT; i++) {
    result = sendHTTPReq(senssorConfigUrl[i]);
    if (result.length() > 0) {
      int idx = result.indexOf("mode");
      if (idx==-1) return;
      result = result.substring(idx+5);
      idx = result.indexOf("<");
      result = result.substring(0, idx);
      mode = mode | result.toInt();   
    }
  }
  Serial.print("Device configured in mode = ");
  Serial.println(mode);
  if ((mode >> ASYNC_MOVE_SENSOR_EN ) & 0x1) 
    Serial.println("Move Sensor Enabled");
  if ((mode >> ASYNC_TEMP_SENSOR_EN ) & 0x1) 
    Serial.println("Temperature Sensor Enabled");
  if ((mode >> DEEP_SLEEP_EN ) & 0x1) 
    Serial.println("Deep Sleep Mode Enabled");
  
}

//------------------------------------------------------------------------------

void setup() {
  Serial.begin(115200); 
  preferences.begin("configuration", false);
  delay(1000);

  pinMode(connectionPin, OUTPUT);
  digitalWrite(connectionPin, LOW);
  
  if (bootCount == 0) {
    //find server only on first boot(deep sleep case)
    int result = findServer();
    // do not configure device if server was not found
    if (result != 0) ESP.restart();
  }

  updateUrls();
  Serial.print("Server IP: ");
  Serial.println(serverIp);      

  if (bootCount == 0) {
    //initialize move sensor state to 0 
    sendHTTPReq(sensorOffUrl);
  }

  //get configuration only on first boot(deep sleep case)
  if (mode == -1)
    getConfiguration();
  
  if ((mode >> ASYNC_TEMP_SENSOR_EN) & 0x1) {
    //ds1820 initilize
    sensors.begin();
  }
  
  
  if ((mode >> DEEP_SLEEP_EN) & 0x1) {
    // deep sleep option
    ++bootCount;
    Serial.println("Boot number: " + String(bootCount));
    task_wakeup_reason();
  
    if ((mode >> ASYNC_TEMP_SENSOR_EN) & 0x1) {
      esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP);
    }
  
    if ((mode >> ASYNC_MOVE_SENSOR_EN) & 0x1) {
      pinMode(sensorPin, INPUT_PULLUP);
      esp_sleep_enable_ext0_wakeup(sensorPin, 1);  
      rtc_gpio_pulldown_dis(sensorPin);
      rtc_gpio_pullup_en(sensorPin);
    }
  }
  else {
  
    if ((mode >> ASYNC_TEMP_SENSOR_EN) & 0x1) {
      timer = timerBegin(1000000); 
      timerAttachInterrupt(timer, &tempTimerInterrupt);
      timerAlarm(timer, TIME_TO_SLEEP, true, 0); 
      tempSensorState = 1;
    }
  
    if ((mode >> ASYNC_MOVE_SENSOR_EN) & 0x1) {
      pinMode(sensorPin, INPUT_PULLUP);
      attachInterrupt(sensorPin, moveSensorInterrupt, RISING);
    }
  }
}

void loop() {
  HTTPClient http;
  
  int result;
  if (moveSensorState == 1) {
    result = sendHTTPReq(sensorOnUrl).length();
    if (result > 0) {
      Serial.println("Move sensor detected");
      delay(10000);
      if (digitalRead(sensorPin) == 0) {
        result = sendHTTPReq(sensorOffUrl).length();
      }
      else result = NOT_READY;
    }

    if (result > 0) {
      moveSensorState = 0;
      Serial.println("Move sensor armed");
    }
  }

  // read ds1820
  if (tempSensorState == 1) {
    sensors.requestTemperatures(); 
    float temperatureC = sensors.getTempCByIndex(0);
    //Serial.println(tempUrl +  String((int)(temperatureC*1000)));
    sendHTTPReq(tempUrl + String((int)(temperatureC*1000)));
    Serial.print("Temperature = ");
    Serial.println(temperatureC);    
    tempSensorState = 0;
  }

  if (moveSensorState == 0 && tempSensorState == 0) {
    if ((mode >> DEEP_SLEEP_EN) & 0x1) {
      Serial.println("Going sleep");
      delay(500);
      esp_deep_sleep_start();
    }
  }
     
}




  
