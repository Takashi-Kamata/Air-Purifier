/*
 *  This sketch sends data via HTTP GET requests to data.sparkfun.com service.
 *
 *  You need to get streamId and privateKey at data.sparkfun.com and paste them
 *  below. Or just customize this script to talk to other HTTP servers.
 *
 */

/*
 * Config
 * Board NodeMCU-32S
 * UP Speed: 9321600
 * Flash Freq: 80MHz
 * None
 * Disabled
 * 
 */

#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Arduino.h>
#include <SensirionI2CSen5x.h>
#include <Wire.h>
#include <WiFi.h>
#include <InfluxDbClient.h>

#define INFLUXDB_URL ""
#define INFLUXDB_TOKEN ""
#define INFLUXDB_ORG ""
#define INFLUXDB_BUCKET ""
// The used commands use up to 48 bytes. On some Arduino's the default buffer
// space is not large enough
#define MAXBUF_REQUIREMENT 48
#if (defined(I2C_BUFFER_LENGTH) &&                 \
     (I2C_BUFFER_LENGTH >= MAXBUF_REQUIREMENT)) || \
    (defined(BUFFER_LENGTH) && BUFFER_LENGTH >= MAXBUF_REQUIREMENT)
#define USE_PRODUCT_INFO
#endif

// WiFi network name and password:
const char *host = "";
const char *ssid = "";
const char *password = "";

unsigned long previousMillis = 0;
unsigned long interval = 30000;
WebServer server(80);

// Single InfluxDB instance
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);

// PWM settings
int pwmChannel = 0; // Selects channel 0
int frequence = 70; // PWM frequency of 1 KHz
int resolution = 8; // 8-bit resolution, 256 possible values
int pwmPin = 32;    // Pin to be used for PWM

// Web page to handle uploading a new binary file to the ESP32
const char *serverIndex =
    "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
    "<form action='/' method='POST' name='fan'>"
    "<input type='number' name='userInput'  min='5' max='2000'>"
    "<input type='submit' value='Submit'>"
    "</form>"
    "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form' name='binary'>"
    "<input type='file' name='update'>"
    "<input type='submit' value='Update'>"
    "</form>"
    "<div id='prg'>progress: 0%</div>"
    "<script>"
    "$('#upload_form').submit(function(e){"
    "e.preventDefault();"
    "var form = $('#upload_form')[0];"
    "var data = new FormData(form);"
    " $.ajax({"
    "url: '/update',"
    "type: 'POST',"
    "data: data,"
    "contentType: false,"
    "processData:false,"
    "xhr: function() {"
    "var xhr = new window.XMLHttpRequest();"
    "xhr.upload.addEventListener('progress', function(evt) {"
    "if (evt.lengthComputable) {"
    "var per = evt.loaded / evt.total;"
    "$('#prg').html('progress: ' + Math.round(per*100) + '%');"
    "}"
    "}, false);"
    "return xhr;"
    "},"
    "success:function(d, s) {"
    "console.log('success!')"
    "},"
    "error: function (a, b, c) {"
    "}"
    "});"
    "});"
    "</script>";

// Create SEN55 instance
SensirionI2CSen5x sen5x;

void fail()
{
  while (1)
  {
    delay(1);
  }
}

void printSEN55Versions()
{
  uint16_t error;
  char errorMessage[256];

  unsigned char productName[32];
  uint8_t productNameSize = 32;

  error = sen5x.getProductName(productName, productNameSize);

  if (error)
  {
    Serial.print("Error trying to execute getProductName(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    fail();
  }
  else
  {
    Serial.print("ProductName:");
    Serial.println((char *)productName);
  }

  uint8_t firmwareMajor;
  uint8_t firmwareMinor;
  bool firmwareDebug;
  uint8_t hardwareMajor;
  uint8_t hardwareMinor;
  uint8_t protocolMajor;
  uint8_t protocolMinor;

  error = sen5x.getVersion(firmwareMajor, firmwareMinor, firmwareDebug,
                           hardwareMajor, hardwareMinor, protocolMajor,
                           protocolMinor);
  if (error)
  {
    Serial.print("Error trying to execute getVersion(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    fail();
  }
  else
  {
    Serial.print("Firmware: ");
    Serial.print(firmwareMajor);
    Serial.print(".");
    Serial.print(firmwareMinor);
    Serial.print(", ");

    Serial.print("Hardware: ");
    Serial.print(hardwareMajor);
    Serial.print(".");
    Serial.println(hardwareMinor);
  }
}

void printSEN55SerialNumber()
{
  uint16_t error;
  char errorMessage[256];
  unsigned char serialNumber[32];
  uint8_t serialNumberSize = 32;

  error = sen5x.getSerialNumber(serialNumber, serialNumberSize);
  if (error)
  {
    Serial.print("Error trying to execute getSerialNumber(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    fail();
  }
  else
  {
    Serial.print("SerialNumber:");
    Serial.println((char *)serialNumber);
  }
}

void setup()
{
  Serial.begin(115200);

  // Blue LED setup
  pinMode(LED_BUILTIN, OUTPUT);

  // Setup PWM
  // Configuration of channel 0 with the chosen frequency and resolution
  ledcSetup(pwmChannel, 10, resolution);
  // Assigns the PWM channel to pin 23
  ledcAttachPin(pwmPin, pwmChannel);
  // Create the selected output voltage
  ledcWrite(pwmChannel, 127); // 1.65 V -> require logic shifter to shift to 2.5V for the fan

  // Connect to a WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // MDNS for host name
  if (!MDNS.begin(host))
  { // http://airpurifier.local
    Serial.println("Error setting up MDNS responder!");
    while (1)
    {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");

  server.on("/", HTTP_GET, []()
            {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex); });

  /*handling uploading firmware file */
  server.on(
      "/update", HTTP_POST, []()
      {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
      ESP.restart(); },
      []()
      {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START)
        {
          Serial.printf("Update: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN))
          { // start with max available size
            Update.printError(Serial);
          }
        }
        else if (upload.status == UPLOAD_FILE_WRITE)
        {
          /* flashing firmware to ESP*/
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
          {
            Update.printError(Serial);
          }
        }
        else if (upload.status == UPLOAD_FILE_END)
        {
          if (Update.end(true))
          { // true to set the size to the current progress
            Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
          }
          else
          {
            Update.printError(Serial);
          }
        }
      });

  server.on("/", HTTP_POST, []()
            {
    String userInput = server.arg("userInput");
    frequence = userInput.toInt();
    // if (frequence >= 70 && frequence <= 700)
    // {
      // Valid input, do something with it
      Serial.print("Setting frequency: ");
      Serial.println(frequence);
      ledcChangeFrequency(pwmChannel, frequence, resolution);
      server.send(200, "text/html", serverIndex);
    // }
    // else
    // {
    //   // Invalid input, send an error message
    //   server.send(400, "text/plain", "Invalid value. Please enter a number between 70 and 700.");
    // } 
    });
  server.begin();

  Wire.begin(23, 22); // SDA, SCL
  if (!client.validateConnection())
  {
    Serial.print("Could not connect to InfluxDB");
    fail();
  }

  sen5x.begin(Wire);
  uint16_t error;
  char errorMessage[256];
  error = sen5x.deviceReset();
  if (error)
  {
    Serial.print("Error trying to execute deviceReset(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    fail();
  }

// Print SEN55 module information if i2c buffers are large enough
#ifdef USE_PRODUCT_INFO
  printSEN55SerialNumber();
  printSEN55Versions();
#endif

  // For device's self-heat compensation
  float tempOffset = 0.0;
  error = sen5x.setTemperatureOffsetSimple(tempOffset);
  if (error)
  {
    Serial.print("Error trying to execute setTemperatureOffsetSimple(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    fail();
  }
  else
  {
    Serial.print("Temperature Offset set to ");
    Serial.print(tempOffset);
    Serial.println(" deg. Celsius (SEN54/SEN55 only");
  }
  uint32_t cleaning_interval = 0;
  uint16_t cleaning_state = sen5x.getFanAutoCleaningInterval(cleaning_interval);
  Serial.print("Cleaning Information getFanAutoCleaningInterval(): ");
  Serial.println(cleaning_interval);
  sen5x.startFanCleaning();
  Serial.println("Fan cleaning");

  // Start Measurement
  error = sen5x.startMeasurement();
  if (error)
  {
    Serial.print("Error trying to execute startMeasurement(): ");
    errorToString(error, errorMessage, 256);
    Serial.println(errorMessage);
    fail();
  }
}

unsigned long previousMillisSEN55 = 0;

void loop()
{
  server.handleClient();

  unsigned long currentMillis = millis();
  // if WiFi is down, try reconnecting every CHECK_WIFI_TIME seconds
  if ((WiFi.status() != WL_CONNECTED) && (currentMillis - previousMillis >=interval)) {
    Serial.print(millis());
    Serial.println("Reconnecting to WiFi...");
    WiFi.disconnect();
    WiFi.reconnect();
    previousMillis = currentMillis;
  } else {
    if (currentMillis - previousMillisSEN55 >= 1000) {
      uint16_t error;
      char errorMessage[256];
      delay(1);
      bool ready;
      sen5x.readDataReady(ready);
      if (ready)
      {
        digitalWrite(LED_BUILTIN, HIGH);
        // Read Measurement
        float massConcentrationPm1p0;
        float massConcentrationPm2p5;
        float massConcentrationPm4p0;
        float massConcentrationPm10p0;
        float temp_ambientHumidity;
        float temp_ambientTemperature;
        float temp_vocIndex;
        float temp_noxIndex;

        error = sen5x.readMeasuredValues(
            massConcentrationPm1p0, massConcentrationPm2p5, massConcentrationPm4p0,
            massConcentrationPm10p0, temp_ambientHumidity, temp_ambientTemperature, temp_vocIndex,
            temp_noxIndex);

        if (error)
        {
          Serial.print("Error trying to execute readMeasuredValues(): ");
          errorToString(error, errorMessage, 256);
          Serial.println(errorMessage);
        }
        else
        {
          Point pointDevice("sen55");
          // Assumes when the sensor is ready, it has valid PM data
          pointDevice.addField("pm2.5", massConcentrationPm2p5);
          pointDevice.addField("pm1", massConcentrationPm1p0);
          pointDevice.addField("pm4", massConcentrationPm4p0);
          pointDevice.addField("pm10", massConcentrationPm10p0);
          float ambientHumidity;
          float ambientTemperature;
          float vocIndex;
          float noxIndex;

          if (!isnan(temp_ambientHumidity))
          {
            pointDevice.addField("humidity", temp_ambientHumidity);
            ambientHumidity = temp_ambientHumidity;
          }
          if (!isnan(temp_ambientTemperature))
          {
            pointDevice.addField("temperature", temp_ambientTemperature);
            ambientTemperature = temp_ambientTemperature;
          }
          if (!isnan(temp_vocIndex))
          {
            pointDevice.addField("voc", temp_vocIndex);
            vocIndex = temp_vocIndex;
          }
          if (!isnan(temp_noxIndex))
          {
            pointDevice.addField("nox", temp_noxIndex);
            noxIndex = temp_noxIndex;
          }
          pointDevice.addField("pwm", frequence);
          ledcChangeFrequency(pwmChannel, set_pwm, resolution);
          if (!client.writePoint(pointDevice))
          {
            Serial.print("InfluxDB write failed");
          }
        }
      }
      previousMillisSEN55 = currentMillis;
    }
  }
}
