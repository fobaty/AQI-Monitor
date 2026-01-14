#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_SCD30.h>

/* ===== DISPLAY ===== */
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

/* ================= CONFIG ================= */
const char* AP_SSID = "AIR-SCAN";
const char* AP_PASS = "12345678";

/* I2C pins */
#define I2C_SDA 12
#define I2C_SCL 13

/* PMS5003 UART */
#define PMS_RX 11
#define PMS_TX 10

/* ST7735 SPI pins */
#define TFT_SCK  1
#define TFT_MOSI 2
#define TFT_RST  3
#define TFT_DC   4
#define TFT_CS   5
#define TFT_BL   6
/* ========================================== */

WebServer server(80);
Adafruit_SCD30 scd30;
HardwareSerial PMS(1);

/* Display */
SPIClass spiTFT(FSPI);
Adafruit_ST7735 tft = Adafruit_ST7735(&spiTFT, TFT_CS, TFT_DC, TFT_RST);
unsigned long displayInterval = 5000;

/* -------- Sensor states -------- */
bool scd30Detected = false;
bool pmsDetected = false;

/* -------- Measurements -------- */
float co2 = NAN;
float temperature = NAN;
float humidity = NAN;

uint16_t pm1 = 0;
uint16_t pm25 = 0;
uint16_t pm10 = 0;
unsigned long lastSerial = 0;
unsigned long lastDisplay = 0;


/* ===== Level helpers ===== */
String pmLevel(uint16_t v) {
  if (v <= 12) return "Good";
  if (v <= 35) return "Moderate";
  if (v <= 55) return "Unhealthy";
  return "Hazardous";
}

String co2Level(float v) {
  if (v <= 800) return "Good";
  if (v <= 1200) return "Moderate";
  if (v <= 2000) return "Poor";
  return "Hazardous";
}

String levelColor(String lvl) {
  if (lvl == "Good") return "green";
  if (lvl == "Moderate") return "orange";
  if (lvl == "Unhealthy" || lvl == "Poor") return "red";
  return "darkred";
}

/* ================= HTML ================= */
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Air Quality Monitor</title>
<style>
.box { padding:10px; margin:6px; border-radius:6px; color:white; }
.data { font-size:18px; }
</style>
</head>
<body>
<h2>Air Quality Monitor</h2>

<div id="pm1" class="box">PM1.0</div>
<div id="pm25" class="box">PM2.5</div>
<div id="pm10" class="box">PM10</div>
<div id="co2" class="box">CO₂</div>

<h3>Climate (SCD30)</h3>
<p class="data">Temperature: <span id="temp">-</span> °C</p>
<p class="data">Humidity: <span id="hum">-</span> %</p>

<script>
function update(){
 fetch("/status").then(r=>r.json()).then(d=>{
  setBox("pm1", d.pm1, d.pm1lvl, d.pm1clr);
  setBox("pm25", d.pm25, d.pm25lvl, d.pm25clr);
  setBox("pm10", d.pm10, d.pm10lvl, d.pm10clr);
  setBox("co2", d.co2, d.co2lvl, d.co2clr);

  document.getElementById("temp").innerText = d.temp ?? "-";
  document.getElementById("hum").innerText  = d.hum ?? "-";
 });
}

function setBox(id,val,lvl,clr){
 let e=document.getElementById(id);
 e.style.background=clr;
 e.innerText = id.toUpperCase()+": "+val+" ("+lvl+")";
}

update();
setInterval(update,5000);
</script>
</body>
</html>
)rawliteral";

/* ========================================= */

bool readPMS() {
  if (PMS.available() < 32) return false;
  if (PMS.read() != 0x42) return false;
  if (PMS.read() != 0x4D) return false;

  uint8_t buf[30];
  PMS.readBytes(buf, 30);

  pm1  = buf[8]  << 8 | buf[9];
  pm25 = buf[10] << 8 | buf[11];
  pm10 = buf[12] << 8 | buf[13];

  pmsDetected = true;
  return true;
}

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  delay(500);

 /* -------- Display -------- */
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  spiTFT.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_GREENTAB);
  tft.setRotation(0);
  tft.fillScreen(ST77XX_BLACK);

  /* -------- Splash Screen -------- */
  tft.drawRoundRect(5, 5, tft.width() - 10, 45, 8, ST77XX_CYAN);
  
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(20, 15);
  tft.print("AIR");
  tft.setTextColor(ST77XX_WHITE);
  tft.print(" SCAN");
  
  tft.setTextSize(1);
  tft.setCursor(20, 35);
  tft.print("Esp32s3-Monitor");

  tft.drawFastHLine(10, 60, tft.width() - 20, ST77XX_ORANGE);

  tft.setCursor(10, 75);
  tft.setTextColor(ST77XX_WHITE);
  tft.println("SYSTEM CHECK:");
  
  auto printStatus = [](const char* msg, bool ok) {
    tft.print(" > ");
    tft.print(msg);
    tft.print(":");
    tft.setCursor(95, tft.getCursorY());
    if (ok) {
      tft.setTextColor(ST77XX_GREEN);
      tft.println("OK");
    } else {
      tft.setTextColor(ST77XX_RED);
      tft.println("FAIL");
    }
    tft.setTextColor(ST77XX_WHITE);
    delay(1000);
  };

  /* -------- The sensors are initialized right during the splash screen-------- */
  tft.setCursor(10, 90);
  
  // 1. I2C и SCD30
  Wire.begin(I2C_SDA, I2C_SCL);
  scd30Detected = scd30.begin();
  printStatus("SCD30", scd30Detected);

  // 2. PMS5003
  PMS.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  printStatus("PMS5003", true); // UART всегда true при инициализации

  // 3.  WiFi
  WiFi.softAP(AP_SSID, AP_PASS);
  printStatus("WIFI AP", true);

  // 4. Web server
  server.on("/", [](){ server.send_P(200,"text/html",INDEX_HTML); });
  server.on("/status", [](){
    String json="{";
    json += "\"pm1\":"+String(pm1)+",";
    json += "\"pm1lvl\":\""+pmLevel(pm1)+"\",";
    json += "\"pm1clr\":\""+levelColor(pmLevel(pm1))+"\",";
    json += "\"pm25\":"+String(pm25)+",";
    json += "\"pm25lvl\":\""+pmLevel(pm25)+"\",";
    json += "\"pm25clr\":\""+levelColor(pmLevel(pm25))+"\",";
    json += "\"pm10\":"+String(pm10)+",";
    json += "\"pm10lvl\":\""+pmLevel(pm10)+"\",";
    json += "\"pm10clr\":\""+levelColor(pmLevel(pm10))+"\",";
    json += "\"co2\":"+String(co2)+",";
    json += "\"co2lvl\":\""+co2Level(co2)+"\",";
    json += "\"co2clr\":\""+levelColor(co2Level(co2))+"\",";
    json += "\"temp\":"+(isnan(temperature)?"null":String(temperature))+",";
    json += "\"hum\":" +(isnan(humidity)?"null":String(humidity));
    json += "}";
    server.send(200,"application/json",json);
  });
  server.begin();
  printStatus("SERVER", true);

  // Final message
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(15, 145);
  tft.println("STARTING ENGINE...");
  delay(1500);

  tft.fillScreen(ST77XX_BLACK);
  lastDisplay = millis(); // Progress bar timer reset
}

void loop() {
  server.handleClient();

  /* -------- SCD30 -------- */
  if (scd30Detected && scd30.dataReady()) {
    scd30.read();
    co2 = scd30.CO2;
    temperature = scd30.temperature;
    humidity = scd30.relative_humidity;
  }

  /* -------- PMS5003 -------- */
  readPMS();

  /* -------- SERIAL OUTPUT & TEXT UPDATE -------- */
  if (millis() - lastSerial >= 5000) {
    lastSerial = millis();
    lastDisplay = millis();

    Serial.println("\n====== AQI MEASUREMENTS ======");

    if (scd30Detected) {
      Serial.printf("CO₂   : %.0f ppm [%s]\n", co2, co2Level(co2).c_str());
      Serial.printf("Temp  : %.1f °C\n", temperature);
      Serial.printf("Hum   : %.1f %%\n", humidity);
    } else {
      Serial.println("SCD30 : NOT DETECTED");
    }

    Serial.printf("PM1.0 : %u µg/m³ [%s]\n", pm1,  pmLevel(pm1).c_str());
    Serial.printf("PM2.5 : %u µg/m³ [%s]\n", pm25, pmLevel(pm25).c_str());
    Serial.printf("PM10  : %u µg/m³ [%s]\n", pm10, pmLevel(pm10).c_str());

    Serial.println("================================");

  //TEXT UPDATE
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
    tft.setCursor(30, 0);
    tft.print("Air Quality");
    
    int yOffset = 25;

    // --- CO2 ---
    String co2Lvl = co2Level(co2);
    uint16_t co2Clr = ST77XX_WHITE;
    if (co2Lvl == "Good") co2Clr = ST77XX_GREEN;
    else if (co2Lvl == "Moderate") co2Clr = ST77XX_ORANGE;
    else if (co2Lvl == "Poor") co2Clr = ST77XX_RED;
    else co2Clr = ST77XX_MAGENTA;

    tft.setTextColor(co2Clr, ST77XX_BLACK); 
    tft.setCursor(1, yOffset);
    tft.printf("CO2: %.0f ppm           ", co2); 
    yOffset += 12;
    tft.setCursor(30, yOffset);
    tft.printf("(%s)               ", co2Lvl.c_str()); 
    yOffset += 20;

    // --- Temp & Hum ---
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setCursor(0, yOffset);
    tft.printf("Temp: %.1f C   ", temperature);
    yOffset += 12;
    tft.setCursor(1, yOffset);
    tft.printf("Hum:  %.1f %%   ", humidity);
    yOffset += 20;

    // --- PM1.0 ---
    String pm1Lvl = pmLevel(pm1);
    uint16_t pm1Clr = (pm1Lvl == "Good") ? ST77XX_GREEN : (pm1Lvl == "Moderate") ? ST77XX_ORANGE : ST77XX_RED;
    tft.setTextColor(pm1Clr, ST77XX_BLACK);
    tft.setCursor(1, yOffset);
    tft.printf("PM1.0: %u (%s)          ", pm1, pm1Lvl.c_str());
    yOffset += 16;

    // --- PM2.5 ---
    String pm25Lvl = pmLevel(pm25);
    uint16_t pm25Clr = (pm25Lvl == "Good") ? ST77XX_GREEN : (pm25Lvl == "Moderate") ? ST77XX_ORANGE : ST77XX_RED;
    tft.setTextColor(pm25Clr, ST77XX_BLACK);
    tft.setCursor(1, yOffset);
    tft.printf("PM2.5: %u (%s)          ", pm25, pm25Lvl.c_str());
    yOffset += 16;

    // --- PM10 ---
    String pm10Lvl = pmLevel(pm10);
    uint16_t pm10Clr = (pm10Lvl == "Good") ? ST77XX_GREEN : (pm10Lvl == "Moderate") ? ST77XX_ORANGE : ST77XX_RED;
    tft.setTextColor(pm10Clr, ST77XX_BLACK);
    tft.setCursor(1, yOffset);
    tft.printf("PM10 : %u (%s)          ", pm10, pm10Lvl.c_str());
  } 
  // --- Progress bar ---
  unsigned long timePassed = millis() - lastDisplay;
  
  if (timePassed <= 5000) {

    int barWidth = map(timePassed, 0, 4900, 0, tft.width()); 
    
    if (barWidth > tft.width()) barWidth = tft.width();

    tft.fillRect(0, tft.height() - 2, barWidth, 2, ST77XX_CYAN);
    tft.fillRect(barWidth, tft.height() - 2, tft.width() - barWidth, 2, ST77XX_BLACK);
  }
  
}
