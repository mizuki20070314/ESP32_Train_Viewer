#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "pin_config.h"

// ==========================
//  Display setup
// ==========================
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1,
    LCD_SDIO2, LCD_SDIO3);

#if defined DO0143FAT01
Arduino_GFX *gfx = new Arduino_SH8601(bus, LCD_RST,
                                      0, false, LCD_WIDTH, LCD_HEIGHT);
#elif defined H0175Y003AM || defined DO0143FMST10
Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RST,
                                      0, false, LCD_WIDTH, LCD_HEIGHT,
                                      6, 0, 0, 0);
#else
#error "Unknown display macro. Please define the correct display type."
#endif

// ==========================
//  Wi-Fi setup
// ==========================
const char *ssid = "your_ssid";
const char *password = "your_password";

// ==========================
//  Route URLs
// ==========================
const char *serverURLs[] = {
    "https://www.navitime.co.jp/transfer/searchlist?orvStationName=%E7%AB%AA%E5%A0%80&dnvStationName=%E4%B8%8B%E5%9C%9F%E7%8B%A9",
    "https://www.navitime.co.jp/transfer/searchlist?orvStationName=%E7%AB%AA%E5%A0%80&dnvStationName=%E5%AF%8C%E5%A3%AB",
    "https://www.navitime.co.jp/transfer/searchlist?orvStationName=%E5%AF%8C%E5%A3%AB&dnvStationName=%E4%B8%8B%E5%9C%9F%E7%8B%A9",
    "https://www.navitime.co.jp/transfer/searchlist?orvStationName=%E6%B2%BC%E6%B4%A5&dnvStationName=%E4%B8%8B%E5%9C%9F%E7%8B%A9",
    "https://www.navitime.co.jp/transfer/searchlist?orvStationName=%E4%B8%8B%E5%9C%9F%E7%8B%A9&dnvStationName=%E7%AB%AA%E5%A0%80",
    "https://www.navitime.co.jp/transfer/searchlist?orvStationName=%E4%B8%8B%E5%9C%9F%E7%8B%A9&dnvStationName=%E6%B2%BC%E6%B4%A5",
    "https://www.navitime.co.jp/transfer/searchlist?orvStationName=%E6%B2%BC%E6%B4%A5&dnvStationName=%E7%AB%AA%E5%A0%80",
    "https://www.navitime.co.jp/transfer/searchlist?orvStationName=%E5%AF%8C%E5%A3%AB&dnvStationName=%E7%AB%AA%E5%A0%80"};

const char *routeLabels[] = {
    "Tatebori -> Shimotogari",
    "Tatebori -> Fuji",
    "Fuji -> Shimotogari",
    "Numazu -> Shimotogari",
    "Shimotogari -> Tatebori",
    "Shimotogari -> Numazu",
    "Numazu -> Tatebori",
    "Fuji -> Tatebori"};

const int numRoutes = sizeof(serverURLs) / sizeof(serverURLs[0]);
int currentRoute = 0;

// ==========================
//  Button setup
// ==========================
const int buttonPin = 0;
bool lastButtonState = HIGH;
unsigned long buttonPressTime = 0;
bool longPressHandled = false;

// ==========================
//  Schedule index & time
// ==========================
int departureIndex = 0;
time_t targetTime = 0;

// ==========================
//  Delay info (added)
// ==========================
bool lastDelayState = false;         // 前回描画状態
String lastDelayDetail = "";         // 遅延詳細（シリアル出力用）

// ==========================
//  Function declarations
// ==========================
bool fetchRoute(int index);
String getTrainDelayDetail(const String &url);

bool flag = true;

const int   BAT_PIN        = 4;
const float DIVIDER_RATIO  = 3.0f;   // (R12 + R17) / R17
const int   NUM_SAMPLES    = 16;     // 簡易平均
// Li-ion 1セル (最大4.2V程度) 想定: 6dB
// 5V超などもっと高い電圧なら ADC_11db に変更
const adc_attenuation_t ATTENUATION = ADC_6db;

// ==========================
//  Setup
// ==========================
void setup()
{
    Serial.begin(115200);
    analogSetPinAttenuation(BAT_PIN, ATTENUATION);
    pinMode(buttonPin, INPUT_PULLUP);
    pinMode(LCD_EN, OUTPUT);
    digitalWrite(LCD_EN, LOW);
    delay(100);
    digitalWrite(LCD_EN, HIGH);

    if (!gfx->begin())
    {
        Serial.println("Display init failed!");
        while (1)
            ;
    }

    gfx->fillScreen(BLACK);
    gfx->setTextSize(4);
    gfx->setTextColor(GREEN);

    Serial.println("Connecting WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    configTime(9 * 3600, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo))
    {
        Serial.println("Getting NTP time...");
        delay(500);
    }
    Serial.println("NTP time OK!");

    fetchRoute(departureIndex);
}

// ==========================
//  Main loop
// ==========================
void loop()
{
    bool buttonState = digitalRead(buttonPin);

    // short press → next route
    if (buttonState == LOW && lastButtonState == HIGH)
    {
        buttonPressTime = millis();
        longPressHandled = false;
    }
    else if (buttonState == LOW && !longPressHandled && millis() - buttonPressTime > 1000)
    {
        departureIndex++;
        if (!fetchRoute(departureIndex))
        {
            departureIndex = 0;
            fetchRoute(departureIndex);
        }
        longPressHandled = true;
        flag = true;
    }
    else if (buttonState == HIGH && lastButtonState == LOW && !longPressHandled)
    {
        currentRoute = (currentRoute + 1) % numRoutes;
        departureIndex = 0;
        fetchRoute(departureIndex);
        flag = true;
    }
    lastButtonState = buttonState;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        static unsigned long lastUpdate = 0;
        if (millis() - lastUpdate > 500)
        {
            lastUpdate = millis();
            char timeStr[16];
            strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
            gfx->fillRect(0, 60, LCD_WIDTH, 30, BLACK);
            gfx->setTextColor(ORANGE);
            gfx->setCursor(140, 60);
            gfx->print(timeStr);
            float vbat = readBatteryVoltage();
            int p = batterySoC_int(vbat);
            Serial.printf("Battery: %.3f V  %d %%\n", vbat, p);
            gfx->fillRect(0, 420, LCD_WIDTH, 15, BLACK);
            gfx->setTextColor(WHITE);
            gfx->setCursor(160,420);
            gfx->setTextSize(2);
            gfx->printf("BAT %.2fV %d%%", vbat, p);
            gfx->setTextSize(4);

            if (targetTime > 0 && flag == true)
            {
                time_t now = mktime(&timeinfo);
                int diff = difftime(targetTime, now);
                int min = diff / 60;
                if(min == 1439) {
                  min = 0;
                  flag = false;
                }
                gfx->fillRect(0, 250, LCD_WIDTH, 30, BLACK);
                gfx->setTextColor(RED);
                gfx->setCursor(30, 250);
                gfx->printf("Depart in %d min!", min);
            }
        }
    }

    gfx->setTextSize(2);
    gfx->setTextColor(WHITE);
    gfx->setCursor(80, 350);
    gfx->printf("ESP32 Train Schedule Viewer");
    gfx->setCursor(130, 370);
    gfx->printf("Developed by Mizuki");
    gfx->setCursor(120, 390);
    gfx->printf("It was really hard...");
    gfx->setTextSize(4);

    if (millis() - buttonPressTime > 120000)
    {
        digitalWrite(LCD_EN, LOW);
        delay(100);
        esp_deep_sleep_start();
    }
    
}


float readBatteryVoltage() {
  long sum_mv = 0;
  for (int i = 0; i < NUM_SAMPLES; ++i) {
    // 内部でキャリブレーション (eFuse/特性補正) を使って mV に変換
    int mv = analogReadMilliVolts(BAT_PIN);
    sum_mv += mv;
    delay(2); // ノイズ低減用のごく短い間隔
  }
  float v_adc_mv = (float)sum_mv / NUM_SAMPLES;   // 分圧点の電圧[mV]
  float v_bat_mv = v_adc_mv * DIVIDER_RATIO;      // 元のバッテリ電圧[mV]
  return v_bat_mv / 1000.0f;                      // V
}

float batterySoC_percent(float v) {
  const float Vmax = 4.00f;
  const float Vbrk = 3.60f;
  const float Vmin = 2.75f;
  const float Pbrk = 10.0f; // 3.60Vで10%

  if (v <= Vmin) return 0.0f;
  if (v >= Vmax) return 100.0f;

  const float m_high = (100.0f - Pbrk) / (Vmax - Vbrk); 
  const float m_low  = Pbrk / (Vbrk - Vmin);            

  if (v < Vbrk) {
    return m_low * (v - Vmin);          // 0～10%
  } else {
    return Pbrk + m_high * (v - Vbrk);  // 10～100%
  }
}

// 四捨五入して 0～100 の整数 % を得る
uint8_t batterySoC_int(float v) {
  float p = batterySoC_percent(v);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return (uint8_t)roundf(p);
}

// ==========================
//  Fetch route info + delay
// ==========================
bool fetchRoute(int index)
{
    Serial.printf("\n=== Route change ===\nCurrent: %s\n", routeLabels[currentRoute]);
    HTTPClient http;
    http.begin(serverURLs[currentRoute]);
    int httpCode = http.GET();
    Serial.printf("HTTP code: %d\n", httpCode);

    if (httpCode <= 0)
    {
        Serial.println("HTTP request failed");
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    String startTag = "<dt class=\"left\">";
    String endTag = "</dt>";
    int startIndex = 0;
    int count = 0;
    const int maxCount = 4;
    String times[maxCount];

    while (count < maxCount && (startIndex = payload.indexOf(startTag, startIndex)) != -1)
    {
        startIndex += startTag.length();
        int endIndex = payload.indexOf(endTag, startIndex);
        if (endIndex == -1)
            break;

        String timeStr = payload.substring(startIndex, endIndex);
        timeStr.replace("&nbsp;", " ");
        String test = timeStr;
        test.replace(":", "");
        if (timeStr.length() - test.length() == 2)
        {
            times[count++] = timeStr;
        }

        startIndex = endIndex + endTag.length();
    }

    if (index >= count)
    {
        Serial.printf("Only %d trains available.\n", count);
        return false;
    }

    String selectedTime = times[index];
    Serial.printf("Selected: %d: %s\n", index + 1, selectedTime.c_str());

    // ===== Display drawing =====
    gfx->fillScreen(BLACK);

    // routeLabels -> split by " -> "
    String route = routeLabels[currentRoute];
    int arrowPos = route.indexOf("->");
    String from = route.substring(0, arrowPos);
    from.trim();
    String to = route.substring(arrowPos + 2);
    to.trim();

    gfx->setTextColor(GREEN);
    gfx->setCursor(70, 110);
    gfx->print(from);
    gfx->setCursor(180, 150);
    gfx->print(to);

    gfx->setTextColor(ORANGE);
    gfx->setCursor(60, 200);
    String displayTime = selectedTime;
    displayTime.replace("⇒", "->");
    gfx->print(displayTime);

    int hour = selectedTime.substring(0, 2).toInt();
    int minute = selectedTime.substring(3, 5).toInt();
    int second = selectedTime.substring(6, 8).toInt();

    struct tm now;
    getLocalTime(&now);
    time_t nowTime = mktime(&now);

    struct tm target = now;
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = second;
    targetTime = mktime(&target);
    if (targetTime < nowTime)
    {
        target.tm_mday++;
        targetTime = mktime(&target);
    }

    // ===== Delay detection (added) =====
    const String delayNotice = "遅延・運転見合わせが発生中";
    bool hasDelay = false;
    String delayLink = "";
    int delayIndex = payload.indexOf(delayNotice);
    if (delayIndex != -1)
    {
        hasDelay = true;
        // Extract link
        int linkStart = payload.indexOf("<a href=\"", delayIndex);
        if (linkStart != -1)
        {
            linkStart += 9;
            int linkEnd = payload.indexOf("\"", linkStart);
            if (linkEnd != -1)
            {
                delayLink = payload.substring(linkStart, linkEnd);
                delayLink.replace("&amp;", "&");
                if (delayLink.startsWith("//"))
                    delayLink = "https:" + delayLink;
                Serial.print("Delay link: ");
                Serial.println(delayLink);
            }
        }
    }

    // Draw delay status
    gfx->fillRect(0, 10, LCD_WIDTH, 40, BLACK);
    if (hasDelay)
    {
        gfx->setTextColor(YELLOW);
        gfx->setCursor(155, 300);
        gfx->print("Delayed");
    }
    else
    {
        gfx->setTextColor(DARKGREEN);
        gfx->setCursor(110, 300);
        gfx->print("Not Delayed");
    }

    // Fetch detail only if delay and link found
    if (hasDelay && delayLink.length())
    {
        String detail = getTrainDelayDetail(delayLink);
        lastDelayDetail = detail;
        Serial.println("[Delay detail] " + detail);
    }
    else
    {
        lastDelayDetail = "";
    }
    lastDelayState = hasDelay;

    return true;
}

// ==========================
//  Delay detail fetch (HTTPS)
// ==========================
String getTrainDelayDetail(const String &url)
{
    WiFiClientSecure client;
    client.setInsecure(); // Skip certificate validation

    HTTPClient https;
    if (!https.begin(client, url))
    {
        return "HTTPS begin failed";
    }

    int code = https.GET();
    if (code != HTTP_CODE_OK)
    {
        https.end();
        return "HTTP error: " + String(code);
    }

    String payload = https.getString();
    https.end();

    // Extract reason
    String reason;
    String targetTag = "<dd class=\"traininfo-detail\">";
    int tagStart = payload.indexOf(targetTag);
    if (tagStart != -1)
    {
        int contentStart = tagStart + targetTag.length();
        int contentEnd = payload.indexOf("</dd>", contentStart);
        if (contentEnd != -1)
        {
            reason = payload.substring(contentStart, contentEnd);
            reason.trim();
        }
    }
    if (reason.length() == 0)
        reason = "理由不明";

    // Extract line name from <h1>
    String lineName;
    String h1Tag = "<h1>";
    int h1Start = payload.indexOf(h1Tag);
    if (h1Start != -1)
    {
        h1Start += h1Tag.length();
        int h1End = payload.indexOf("</h1>", h1Start);
        if (h1End != -1)
        {
            String h1Content = payload.substring(h1Start, h1End);
            int statusIndex = h1Content.indexOf("運行状況");
            if (statusIndex != -1)
            {
                lineName = h1Content.substring(0, statusIndex);
                lineName.trim();
            }
        }
    }
    if (lineName.length() == 0)
        lineName = "路線不明";

    return lineName + " " + reason;
}
