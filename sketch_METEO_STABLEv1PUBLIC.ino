/*
 * PROJECT: ESP32 METEO STATION (Master Fixed -400 строк супер путер оптимизация)
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_sntp.h>

// драйверы устройств
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>
#include <Adafruit_BME680.h>
#include <WebServer.h>
#include <DNSServer.h>

// ==========================================
// 1. КОНФИГУРАЦИЯ И ПИНЫ
// ==========================================

// --- подключение ---
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define MAX_CS_PIN  5
#define MAX_DIN_PIN 23
#define MAX_CLK_PIN 18
#define POT_PIN     32
#define BTN_RES_PIN 4

// --- настройки оборудования ---
#define SCREEN_W 128
#define SCREEN_H 64
#define ADDR_OLED_WEATHER 0x3C
#define ADDR_OLED_DATE    0x3D
#define ADDR_BME680       0x77
#define MAX_DEVICES       4
#define MAX_HW_TYPE       MD_MAX72XX::FC16_HW

// --- настройки сети ---
#define WIFI_CONF_FILE "/wifi_conf.txt"
#define AP_SSID        "Meteo Station"
#define AP_PASS        "12345678"
#define NTP_SERVER     "pool.ntp.org"
#define GMT_OFFSET     10800 // gmt+3
#define DAYLIGHT_OFFSET 0    // летнее время (0 = выкл)

// --- таймеры (мс) ---
#define T_CLOCK      500UL       // 0.5 сек (точность времени)
#define T_WEATHER    (30*1000UL) // 30 сек (лимит api)
#define T_BME        2000UL      // 2 сек (опрос сенсора)
#define T_WIFI_OUT   30000UL     // 30 сек (таймаут подключения)
#define T_RETRY      10000UL     // 10 сек (повтор при ошибке)

// ==========================================
// 2. ГЛОБАЛЬНЫЕ ОБЪЕКТЫ
// ==========================================

Adafruit_SSD1306 disp_weather(SCREEN_W, SCREEN_H, &Wire, -1);
Adafruit_SSD1306 disp_date(SCREEN_W, SCREEN_H, &Wire, -1);
MD_Parola P = MD_Parola(MAX_HW_TYPE, MAX_DIN_PIN, MAX_CLK_PIN, MAX_CS_PIN, MAX_DEVICES);
Adafruit_BME680 bme;
WebServer server(80);
DNSServer dnsServer;

// --- переменные состояния ---
String wifi_ssid, wifi_pass;
bool is_cfg = false;
bool is_connected = false;
bool is_ap = false;
bool matrix_busy = false;

// --- данные ---
struct tm timeinfo;
bool time_synced = false;
int last_day = -1;
char t_str[6], d_dd[3], d_mm[3], d_yyyy[5];

float bme_t = 0.0, bme_h = 0.0;
int bme_aqi = 0;

struct Weather { String day; String date; float temp; };
Weather forecast[3];
bool weather_ready = false;

// --- метки времени ---
unsigned long last_tick_clk = 0;
unsigned long last_tick_wtr = 0;
unsigned long last_tick_bme = 0;
unsigned long wifi_start_ts = 0;
unsigned long matrix_timer = 0;

enum WfState { WF_DIS, WF_CON, WF_OK, WF_FAIL };
WfState wf_state = WF_DIS;

// ==========================================
// 3. HTML СТРАНИЧКА
// ==========================================

const char *html_index = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><title>Setup</title><style>:root{--bg:#000;--card:#1c1c1e;--txt:#fff;--sub:#86868b;--brd:#38383a;--btn:#fff;--btnt:#000}body{font-family:-apple-system,BlinkMacSystemFont,"SF Pro Text",Roboto,sans-serif;background:var(--bg);color:var(--txt);margin:0;display:flex;justify-content:center;align-items:center;height:100vh}.box{width:90%;max-width:360px;padding:20px;text-align:center}h1{font-size:28px;font-weight:700;margin-bottom:8px}p{color:var(--sub);font-size:15px;margin:0 0 40px;line-height:1.4}form{display:flex;flex-direction:column;gap:16px}input{width:100%;background:var(--card);border:1px solid var(--brd);border-radius:12px;padding:16px;font-size:17px;color:var(--txt);box-sizing:border-box;outline:0;transition:0.2s}input:focus{border-color:#666}button{width:100%;background:var(--btn);color:var(--btnt);border:0;border-radius:12px;padding:16px;font-size:17px;font-weight:600;cursor:pointer;margin-top:10px;transition:0.2s}button:active{opacity:0.7}.foot{margin-top:40px;font-size:12px;color:#333}</style></head><body><div class="box"><h1>Welcome</h1><p>Enter Wi-Fi credentials to connect the Station.</p><form method="POST" action="/save"><input type="text" name="s" placeholder="SSID" required autocomplete="off"><input type="password" name="p" placeholder="Password" required><button type="submit">Connect</button></form><div class="foot">Designed for ESP32</div></div></body></html>
)rawliteral";

const char *html_ok = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><style>body{font-family:-apple-system,sans-serif;background:#000;color:#fff;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;text-align:center}h1{font-size:28px}p{color:#86868b}</style></head><body><div><h1>Success</h1><p>Settings saved.<br>Device restarting...</p></div></body></html>
)rawliteral";

// ==========================================
// 4. СИСТЕМНЫЕ ФУНКЦИИ И СЕТЬ
// ==========================================

void save_cfg() {
  File f = LittleFS.open(WIFI_CONF_FILE, "w");
  if (!f) return;
  f.println(wifi_ssid);
  f.println(wifi_pass);
  f.close();
  is_cfg = true;
}

bool load_cfg() {
  if (!LittleFS.exists(WIFI_CONF_FILE)) return false;
  File f = LittleFS.open(WIFI_CONF_FILE, "r");
  if (!f) return false;
  wifi_ssid = f.readStringUntil('\n'); wifi_ssid.trim();
  wifi_pass = f.readStringUntil('\n'); wifi_pass.trim();
  f.close();
  return (is_cfg = (wifi_ssid.length() > 0));
}

void wipe_cfg() {
  LittleFS.remove(WIFI_CONF_FILE);
  is_cfg = false;
}

// обработчики сервера
void h_root() { server.send(200, "text/html", html_index); }
void h_404()  { 
  server.sendHeader("Location", "http://192.168.4.1/"); 
  server.send(302, "text/plain", "Redirect"); 
}
void h_save() {
  if (server.hasArg("s") && server.hasArg("p")) {
    wifi_ssid = server.arg("s");
    wifi_pass = server.arg("p");
    save_cfg();
    server.send(200, "text/html", html_ok);
    delay(2000);
    ESP.restart();
  } else server.send(400, "text/plain", "Error");
}

void start_ap() {
  if (is_ap) return;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/", h_root);
  server.on("/save", HTTP_POST, h_save);
  server.onNotFound(h_404);
  server.begin();
  is_ap = true;
  wf_state = WF_CON;
  P.displayText("WIFI...", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  matrix_busy = true;
}

void start_sta() {
  if (!is_cfg) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  wf_state = WF_CON;
  wifi_start_ts = millis();
  P.displayText("WIFI...", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  matrix_busy = true;
}

// ==========================================
// 5. ДАННЫЕ И СЕНСОРЫ
// ==========================================

void sync_time() {
  configTime(GMT_OFFSET, DAYLIGHT_OFFSET, NTP_SERVER);
  sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
}

void update_time() {
  if (getLocalTime(&timeinfo)) {
    strftime(t_str, 6, "%H:%M", &timeinfo);
    strftime(d_dd, 3, "%d", &timeinfo);
    strftime(d_mm, 3, "%m", &timeinfo);
    strftime(d_yyyy, 5, "%Y", &timeinfo);
    time_synced = true;
    
    // детектор полуночи для мгновенного обновления даты
    if (last_day != -1 && last_day != timeinfo.tm_mday) last_tick_wtr = 0;
    last_day = timeinfo.tm_mday;
  } else {
    time_synced = false;
  }
  last_tick_clk = millis();
}

void get_weather() {
  if (!is_connected) return;
  HTTPClient http;
  http.begin("https://api.open-meteo.com/v1/forecast?latitude=ВашаШИРОТА&longitude=ВАШАДОЛГОТА&daily=apparent_temperature_mean&timezone=Europe%2FMoscow&forecast_days=3");
  
  if (http.GET() == HTTP_CODE_OK) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, http.getString())) {
      JsonArray tm = doc["daily"]["time"];
      JsonArray tp = doc["daily"]["apparent_temperature_mean"];
      if (tm.size() >= 3 && getLocalTime(&timeinfo)) {
        const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        for (int i=0; i<3; i++) {
          forecast[i].date = tm[i].as<String>().substring(8, 10);
          forecast[i].temp = tp[i];
          forecast[i].day = days[(timeinfo.tm_wday + i) % 7];
        }
        weather_ready = true;
      }
    }
  }
  http.end();
  last_tick_wtr = millis();
}

void read_sensors() {
  if (bme.performReading()) {
    bme_t = bme.temperature;
    bme_h = bme.humidity;
    
    // ступенчатый алгоритм aqi (0-500)
    long gas = (long)(bme.gas_resistance / 1000.0);
    int raw_aqi;
    
    if (gas >= 50) raw_aqi = map(constrain(gas, 50, 500), 50, 500, 50, 0);
    else raw_aqi = map(constrain(gas, 5, 50), 5, 50, 500, 50);
    
    bme_aqi = constrain(raw_aqi, 0, 500);
  }
  last_tick_bme = millis();
}

// ==========================================
// 6. ОТОБРАЖЕНИЕ (UI)
// ==========================================

void draw_weather() {
  disp_weather.clearDisplay();
  if (!is_connected || !time_synced || !weather_ready) {
    disp_weather.setCursor(40, 20); disp_weather.setTextSize(3);
    disp_weather.setTextColor(SSD1306_WHITE); disp_weather.print("--");
  } else {
    int w = SCREEN_W / 3;
    for (int i=0; i<3; i++) {
      int x = i * w + w/2;
      disp_weather.setTextColor(SSD1306_WHITE);
      
      // день
      disp_weather.setTextSize(1);
      int16_t x1, y1; uint16_t ww, hh;
      disp_weather.getTextBounds(forecast[i].day, 0, 0, &x1, &y1, &ww, &hh);
      disp_weather.setCursor(x - ww/2, 2);
      disp_weather.print(forecast[i].day);
      
      // дата
      disp_weather.setTextSize(2);
      disp_weather.getTextBounds(forecast[i].date, 0, 0, &x1, &y1, &ww, &hh);
      disp_weather.setCursor(x - ww/2, 14);
      disp_weather.print(forecast[i].date);
      
      // температура
      String t = String(forecast[i].temp, 1);
      disp_weather.setTextSize(1);
      disp_weather.getTextBounds(t, 0, 0, &x1, &y1, &ww, &hh);
      disp_weather.setCursor(x - ww/2, 32);
      disp_weather.print(t);
      
      if (i < 2) disp_weather.drawFastVLine(x + w/2, 0, SCREEN_H, SSD1306_WHITE);
    }
  }
  disp_weather.display();
}

void draw_date() {
  disp_date.clearDisplay();
  if (!is_connected || !time_synced) {
    disp_date.setCursor(40, 20); disp_date.setTextSize(3);
    disp_date.setTextColor(SSD1306_WHITE); disp_date.print("--");
  } else {
    disp_date.setTextColor(SSD1306_WHITE);
    // дата
    disp_date.setTextSize(2); disp_date.setCursor(5, 5); disp_date.print(d_dd);
    disp_date.setCursor(5, 25); disp_date.print(d_mm);
    disp_date.setTextSize(1); disp_date.setCursor(5, 50); disp_date.print(d_yyyy);
    disp_date.drawFastVLine(55, 0, SCREEN_H, SSD1306_WHITE);
    
    // сенсоры
    disp_date.setTextSize(2); disp_date.setCursor(68, 5); disp_date.print(String(bme_t, 1));
    disp_date.setCursor(68, 25); disp_date.print(String((int)bme_h) + "%");
    disp_date.setTextSize(1); disp_date.setCursor(68, 50); disp_date.print(String(bme_aqi) + " AQI");
  }
  disp_date.display();
}

// задача freertos для плавной яркости
void task_bright(void *p) {
  int last_br = -1;
  for(;;) {
    long sum = 0;
    for(int i=0; i<16; i++) { sum += analogRead(POT_PIN); delay(1); }
    // прямая логика потенциометра
    int val = map(sum/16, 0, 4095, 0, 15);
    val = constrain(val, 0, 15);
    if (val != last_br) { P.setIntensity(val); last_br = val; }
    vTaskDelay(15 / portTICK_PERIOD_MS);
  }
}

// ==========================================
// 7. SETUP & LOOP
// ==========================================

void setup() {
  if (!LittleFS.begin(true)) return;

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  SPI.begin(MAX_CLK_PIN, -1, MAX_DIN_PIN, MAX_CS_PIN);

  if (bme.begin(ADDR_BME680)) {
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
  }

  disp_weather.begin(SSD1306_SWITCHCAPVCC, ADDR_OLED_WEATHER);
  disp_date.begin(SSD1306_SWITCHCAPVCC, ADDR_OLED_DATE);
  
  // гасим дисплеи при старте
  disp_weather.clearDisplay(); disp_weather.display(); disp_weather.ssd1306_command(SSD1306_DISPLAYOFF);
  disp_date.clearDisplay(); disp_date.display(); disp_date.ssd1306_command(SSD1306_DISPLAYOFF);

  P.begin(true); P.displayClear(); P.setIntensity(0);
  P.displayText("BOOT...", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  matrix_busy = true;

  pinMode(POT_PIN, INPUT);
  pinMode(BTN_RES_PIN, INPUT_PULLUP);

  // запускаем задачу яркости на ядре 0
  xTaskCreatePinnedToCore(task_bright, "Bright", 2048, NULL, 1, NULL, 0);

  if (load_cfg()) start_sta();
  else start_ap();
}

void loop() {
  unsigned long now_ms = millis();

  // 1. сеть
  if (is_ap) { dnsServer.processNextRequest(); server.handleClient(); }

  if (wf_state == WF_CON) {
    if (WiFi.status() == WL_CONNECTED) {
      is_connected = true; wf_state = WF_OK;
      if (is_ap) { WiFi.softAPdisconnect(true); server.stop(); dnsServer.stop(); is_ap = false; }
      sync_time(); update_time(); get_weather();
    } else if (now_ms - wifi_start_ts > T_WIFI_OUT) {
      is_connected = false; wf_state = WF_FAIL; matrix_timer = now_ms;
      if (is_cfg) WiFi.disconnect();
    }
  } else if (wf_state == WF_FAIL) {
    if (now_ms > matrix_timer) {
      if (is_cfg) start_sta(); else start_ap();
    }
  } else if (wf_state == WF_OK && WiFi.status() != WL_CONNECTED) {
    is_connected = false; start_sta();
  } else if (wf_state == WF_DIS && is_cfg) {
    start_sta();
  }

  // 2. сброс
  if (!digitalRead(BTN_RES_PIN)) {
    delay(50);
    if (!digitalRead(BTN_RES_PIN)) {
      if (is_cfg) {
        wipe_cfg();
        if (is_connected) { WiFi.disconnect(true); is_connected = false; }
        wf_state = WF_DIS;
      }
      while (!digitalRead(BTN_RES_PIN)) delay(10);
    }
  }

  // 3. обновления
  if (is_connected) {
    if (now_ms - last_tick_clk >= T_CLOCK) update_time();
    
    unsigned long w_int = weather_ready ? T_WEATHER : T_RETRY;
    if (now_ms - last_tick_wtr >= w_int) get_weather();
    
    if (now_ms - last_tick_bme >= T_BME) read_sensors();

    if (getLocalTime(&timeinfo)) {
      disp_weather.ssd1306_command(SSD1306_DISPLAYON);
      disp_date.ssd1306_command(SSD1306_DISPLAYON);
    } else {
      disp_weather.ssd1306_command(SSD1306_DISPLAYOFF);
      disp_date.ssd1306_command(SSD1306_DISPLAYOFF);
    }
    draw_weather();
    draw_date();
  } else {
    disp_weather.ssd1306_command(SSD1306_DISPLAYOFF);
    disp_date.ssd1306_command(SSD1306_DISPLAYOFF);
  }

  // 4. матрица
  if (P.displayAnimate()) matrix_busy = false;
  if (wf_state == WF_OK && !matrix_busy) {
    P.displayText(t_str, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  }
}