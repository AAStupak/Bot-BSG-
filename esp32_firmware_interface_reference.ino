// Reference firmware derived from the legacy SRKLink UI shared by the customer.
// The display assets and encoder handling are preserved for reuse when
// adapting the contactor controller to the Telegram bot integration.
//
// NOTE: This sketch is provided as a standalone reference and is not wired
// into the primary esp32_firmware.ino build used by the bot. It keeps the
// original menu structure, Wi-Fi provisioning helpers, and localized text
// rendering so the styling can be ported while the production firmware keeps
// the REST API expected by the Telegram backend.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ---------------- Display ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- Encoder ----------------
#define ENCODER_S1 25
#define ENCODER_S2 26
#define ENCODER_KEY 27

volatile long encoderPos = 0;
volatile bool encoderPressed = false;
volatile bool longPress = false;
volatile bool encoderMoved = false;
volatile int lastS1State = HIGH;
volatile int lastS2State = HIGH;
volatile unsigned long lastEncoderTime = 0;

volatile bool buttonPressed = false;
volatile unsigned long buttonPressTime = 0;
volatile unsigned long buttonReleaseTime = 0;
volatile bool buttonHandled = false;

const unsigned long ENCODER_DEBOUNCE = 1;
const unsigned long BUTTON_DEBOUNCE = 50;
const unsigned long LONG_PRESS_TIME = 1000;

// ---------------- Animations ----------------
unsigned long lastAnimFrame = 0;
int animFrame = 0;
int slideOffset = 0;
int targetOffset = 0;
bool isSliding = false;

// ---------------- Menu state ----------------
enum MenuState {
  HOME_SCREEN,
  MAIN_MENU,
  WIFI_MENU,
  WIFI_SCAN,
  WIFI_PASSWORD,
  TIME_SETTINGS,
  WIFI_CONNECTING,
  ABOUT_SCREEN,
  LANGUAGE_MENU
};

MenuState currentMenu = HOME_SCREEN;
MenuState previousMenu = HOME_SCREEN;
int menuPosition = 0;

// ---------------- Wi-Fi data ----------------
String wifiNetworks[15];
int wifiSignals[15];
int networkCount = 0;
String selectedSSID;
String wifiPassword;
int passwordCharIndex = 0;
int passwordCursorPos = 0;
bool showPassword = false;

// ---------------- Time ----------------
struct tm timeinfo;
int timeZoneOffset = 2;
bool timeSet = false;
int editTimeMode = 0;
int editHour = 0;
int editMinute = 0;
int editTimezone = 2;

// ---------------- Languages ----------------
enum Language {
  LANG_RUSSIAN = 0,
  LANG_UKRAINIAN = 1,
  LANG_ENGLISH = 2
};

Language currentLanguage = LANG_RUSSIAN;

const char passwordChars[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;:,.<>?/~`";
const int charArraySize = sizeof(passwordChars) - 1;

Preferences preferences;

// ---------------- Cyrillic glyphs (5x8) ----------------
const uint8_t russianFont5x8[][5] PROGMEM = {
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x41, 0x41, 0x7F}, {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x36, 0x08, 0x7F, 0x08, 0x36}, {0x42, 0x41, 0x51, 0x69, 0x46},
    {0x7F, 0x20, 0x18, 0x04, 0x7F}, {0x7F, 0x20, 0x18, 0x04, 0x7F},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x40, 0x40, 0x40, 0x40, 0x7F},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x01, 0x01, 0x01, 0x7F},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x07, 0x08, 0x70, 0x08, 0x07},
    {0x08, 0x3E, 0x49, 0x3E, 0x08}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x7F, 0x40, 0x40, 0x7F, 0x40}, {0x07, 0x08, 0x08, 0x08, 0x7F},
    {0x7F, 0x40, 0x7F, 0x40, 0x7F}, {0x7F, 0x40, 0x7F, 0x40, 0xFF},
    {0x01, 0x7F, 0x49, 0x49, 0x36}, {0x7F, 0x40, 0x3F, 0x49, 0x36},
    {0x7F, 0x48, 0x48, 0x48, 0x30}, {0x22, 0x41, 0x49, 0x49, 0x3E},
    {0x7F, 0x08, 0x3E, 0x41, 0x3E}, {0x46, 0x29, 0x19, 0x09, 0x7F}};

const uint8_t russianSmallFont5x8[][5] PROGMEM = {
    {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7F, 0x50, 0x48, 0x44, 0x38},
    {0x7C, 0x54, 0x54, 0x54, 0x28}, {0x7C, 0x04, 0x04, 0x04, 0x00},
    {0x38, 0x44, 0x44, 0x44, 0x7C}, {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x28, 0x10, 0x7C, 0x10, 0x28}, {0x44, 0x54, 0x54, 0x54, 0x28},
    {0x7C, 0x20, 0x10, 0x08, 0x7C}, {0x7C, 0x20, 0x10, 0x08, 0x7C},
    {0x7C, 0x10, 0x28, 0x44, 0x00}, {0x40, 0x40, 0x40, 0x7C, 0x00},
    {0x7C, 0x04, 0x18, 0x04, 0x7C}, {0x7C, 0x10, 0x10, 0x10, 0x7C},
    {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x04, 0x04, 0x04, 0x7C},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x38, 0x44, 0x44, 0x44, 0x00},
    {0x04, 0x04, 0x7C, 0x04, 0x04}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x10, 0x38, 0x54, 0x38, 0x10}, {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x7C, 0x40, 0x40, 0x7C, 0x40}, {0x0C, 0x10, 0x10, 0x7C, 0x00},
    {0x7C, 0x40, 0x7C, 0x40, 0x7C}, {0x7C, 0x40, 0x7C, 0x40, 0xFC},
    {0x04, 0x7C, 0x54, 0x54, 0x28}, {0x7C, 0x40, 0x38, 0x54, 0x28},
    {0x7C, 0x50, 0x50, 0x50, 0x20}, {0x28, 0x44, 0x54, 0x54, 0x38},
    {0x7C, 0x10, 0x38, 0x44, 0x38}, {0x28, 0x54, 0x34, 0x14, 0x7C}};

const uint8_t ukrainianFont5x8[][5] PROGMEM = {
    {0x7F, 0x09, 0x09, 0x01, 0x03}, {0x3E, 0x49, 0x49, 0x41, 0x22},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x02, 0x01, 0x7F, 0x01, 0x02},
    {0x7C, 0x04, 0x04, 0x00, 0x06}, {0x38, 0x54, 0x56, 0x55, 0x18},
    {0x00, 0x44, 0x7C, 0x40, 0x00}, {0x02, 0x00, 0x7C, 0x00, 0x02}};

// ---------------- Icons ----------------
const unsigned char icon_wifi[] PROGMEM = {
    0x00, 0x00, 0x07, 0xE0, 0x1F, 0xF8, 0x38, 0x1C, 0x63, 0xC6, 0x0F, 0xF0,
    0x1C, 0x38, 0x23, 0xC4, 0x03, 0xC0, 0x06, 0x60, 0x0C, 0x30, 0x01, 0x80,
    0x03, 0xC0, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00};

const unsigned char icon_time[] PROGMEM = {
    0x07, 0xE0, 0x18, 0x18, 0x20, 0x04, 0x47, 0xE2, 0x48, 0x12, 0x90, 0x09,
    0x90, 0x09, 0x90, 0x09, 0x90, 0x09, 0x90, 0x09, 0x48, 0x12, 0x47, 0xE2,
    0x20, 0x04, 0x18, 0x18, 0x07, 0xE0, 0x00, 0x00};

const unsigned char icon_about[] PROGMEM = {
    0x07, 0xE0, 0x0F, 0xF0, 0x1C, 0x38, 0x18, 0x18, 0x30, 0x0C, 0x33, 0xCC,
    0x33, 0xCC, 0x30, 0x0C, 0x30, 0x0C, 0x33, 0xCC, 0x33, 0xCC, 0x30, 0x0C,
    0x18, 0x18, 0x1C, 0x38, 0x0F, 0xF0, 0x07, 0xE0};

// ---------------- Menu definitions ----------------
struct MenuItem {
  const char *title_ru;
  const char *title_ua;
  const char *title_en;
  const unsigned char *icon;
  MenuState nextMenu;
};

MenuItem mainMenuItems[] = {
    {"WiFi", "WiFi", "WiFi", icon_wifi, WIFI_MENU},
    {"Время", "Час", "Time", icon_time, TIME_SETTINGS},
    {"О системе", "Про систему", "About", icon_about, ABOUT_SCREEN}};

// ---------------- Forward declarations ----------------
void IRAM_ATTR handleEncoderS1();
void IRAM_ATTR handleEncoderS2();
void IRAM_ATTR handleButton();
void handleButtonPress();
void handleShortPress();
void handleLongPress();
void handleEncoderChange();
void slideToMenu(MenuState newMenu);
void handleSlideAnimation();
void updateDisplay();
void drawMenu(MenuState menu, int offset);
void drawHomeScreen(int offset);
void drawMainMenu(int offset);
void drawWiFiMenu(int offset);
void drawWiFiScanMenu(int offset);
void drawPasswordMenu(int offset);
void drawTimeSettings(int offset);
void drawConnectingScreen(int offset);
void drawAboutScreen(int offset);
void drawLanguageMenu(int offset);
void drawSignalStrength(int x, int y, int rssi, bool inverted);
void scanWiFiNetworks();
void handlePasswordInput();
void handleTimeSettings();
void saveTimeSettings();
void connectToWiFi();
void resetEncoderPosition(long newPos);
void drawCustomChar(int16_t x, int16_t y, const uint8_t *charData);
void printRussianChar(uint8_t ch);
void printRussianString(const char *str);
String getText(const char *ru_text, const char *ua_text, const char *en_text);
void printText(const char *ru_text, const char *ua_text, const char *en_text);

void setup() {
  Serial.begin(115200);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }
  display.display();
  delay(500);

  pinMode(ENCODER_S1, INPUT_PULLUP);
  pinMode(ENCODER_S2, INPUT_PULLUP);
  pinMode(ENCODER_KEY, INPUT_PULLUP);

  lastS1State = digitalRead(ENCODER_S1);
  lastS2State = digitalRead(ENCODER_S2);

  attachInterrupt(digitalPinToInterrupt(ENCODER_S1), handleEncoderS1, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_S2), handleEncoderS2, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_KEY), handleButton, CHANGE);

  preferences.begin("esp32_settings", false);
  timeZoneOffset = preferences.getInt("timezone", 2);
  currentLanguage = (Language)preferences.getInt("language", LANG_RUSSIAN);

  configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");

  String savedSSID = preferences.getString("ssid", "");
  String savedPassword = preferences.getString("password", "");
  if (savedSSID.length() > 0) {
    WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      timeSet = true;
    }
  }

  currentMenu = HOME_SCREEN;
  updateDisplay();
}

void loop() {
  handleButtonPress();
  if (encoderMoved) {
    encoderMoved = false;
    handleEncoderChange();
  }
  handleSlideAnimation();

  static unsigned long lastTimeUpdate = 0;
  if (millis() - lastTimeUpdate > 1000) {
    lastTimeUpdate = millis();
    if (currentMenu == HOME_SCREEN || currentMenu == TIME_SETTINGS) {
      updateDisplay();
    }
  }

  if (millis() - lastAnimFrame > 200) {
    lastAnimFrame = millis();
    animFrame = (animFrame + 1) % 4;
    if (currentMenu == WIFI_CONNECTING) {
      updateDisplay();
    }
  }

  delay(10);
}

// ---------------- Interrupt handlers ----------------
void IRAM_ATTR handleEncoderS1() {
  unsigned long currentTime = millis();
  if (currentTime - lastEncoderTime < ENCODER_DEBOUNCE)
    return;
  int s1State = digitalRead(ENCODER_S1);
  int s2State = digitalRead(ENCODER_S2);
  if (s1State != lastS1State) {
    if (s1State == LOW) {
      if (s2State == HIGH) {
        encoderPos++;
      } else {
        encoderPos--;
      }
      encoderMoved = true;
      lastEncoderTime = currentTime;
    }
    lastS1State = s1State;
  }
}

void IRAM_ATTR handleEncoderS2() {
  unsigned long currentTime = millis();
  if (currentTime - lastEncoderTime < ENCODER_DEBOUNCE)
    return;
  int s1State = digitalRead(ENCODER_S1);
  int s2State = digitalRead(ENCODER_S2);
  if (s2State != lastS2State) {
    if (s2State == LOW) {
      if (s1State == LOW) {
        encoderPos++;
      } else {
        encoderPos--;
      }
      encoderMoved = true;
      lastEncoderTime = currentTime;
    }
    lastS2State = s2State;
  }
}

void IRAM_ATTR handleButton() {
  unsigned long currentTime = millis();
  bool currentButtonState = digitalRead(ENCODER_KEY);
  if (currentButtonState == LOW && !buttonPressed) {
    if (currentTime - buttonReleaseTime > BUTTON_DEBOUNCE) {
      buttonPressed = true;
      buttonPressTime = currentTime;
      buttonHandled = false;
    }
  } else if (currentButtonState == HIGH && buttonPressed) {
    buttonReleaseTime = currentTime;
    if (!buttonHandled) {
      unsigned long pressDuration = currentTime - buttonPressTime;
      if (pressDuration > LONG_PRESS_TIME) {
        longPress = true;
      } else if (pressDuration > BUTTON_DEBOUNCE) {
        encoderPressed = true;
      }
      buttonHandled = true;
    }
    buttonPressed = false;
  }
}

// ---------------- Input handlers ----------------
void handleButtonPress() {
  if (encoderPressed) {
    encoderPressed = false;
    handleShortPress();
  }
  if (longPress) {
    longPress = false;
    handleLongPress();
  }
}

void handleShortPress() {
  switch (currentMenu) {
  case HOME_SCREEN:
    slideToMenu(MAIN_MENU);
    break;
  case MAIN_MENU:
    slideToMenu(mainMenuItems[menuPosition].nextMenu);
    if (mainMenuItems[menuPosition].nextMenu == WIFI_SCAN) {
      scanWiFiNetworks();
    }
    break;
  case WIFI_MENU:
    if (menuPosition == 0) {
      slideToMenu(WIFI_SCAN);
      scanWiFiNetworks();
    } else {
      slideToMenu(MAIN_MENU);
    }
    break;
  case WIFI_SCAN:
    if (menuPosition < networkCount) {
      selectedSSID = wifiNetworks[menuPosition];
      wifiPassword = "";
      passwordCharIndex = 0;
      passwordCursorPos = 0;
      slideToMenu(WIFI_PASSWORD);
    } else {
      slideToMenu(WIFI_MENU);
    }
    break;
  case WIFI_PASSWORD:
    handlePasswordInput();
    break;
  case TIME_SETTINGS:
    handleTimeSettings();
    break;
  case ABOUT_SCREEN:
    slideToMenu(MAIN_MENU);
    break;
  case WIFI_CONNECTING:
  case LANGUAGE_MENU:
    break;
  }
}

void handleLongPress() {
  switch (currentMenu) {
  case HOME_SCREEN:
    break;
  case WIFI_PASSWORD:
    showPassword = !showPassword;
    updateDisplay();
    break;
  case TIME_SETTINGS:
    if (editTimeMode == 0) {
      editTimeMode = 1;
      getLocalTime(&timeinfo);
      editHour = timeinfo.tm_hour;
      editMinute = timeinfo.tm_min;
      editTimezone = timeZoneOffset;
      resetEncoderPosition(editHour);
    } else {
      editTimeMode = 0;
      saveTimeSettings();
    }
    updateDisplay();
    break;
  default:
    slideToMenu(HOME_SCREEN);
    break;
  }
}

void handleEncoderChange() {
  switch (currentMenu) {
  case MAIN_MENU:
    menuPosition = abs((int)encoderPos) % 3;
    break;
  case WIFI_MENU:
    menuPosition = abs((int)encoderPos) % 2;
    break;
  case WIFI_SCAN:
    if (networkCount > 0) {
      menuPosition = abs((int)encoderPos) % (networkCount + 1);
    }
    break;
  case WIFI_PASSWORD:
    if (passwordCursorPos <= (int)wifiPassword.length()) {
      passwordCharIndex = abs((int)encoderPos) % charArraySize;
    }
    break;
  case TIME_SETTINGS:
    if (editTimeMode == 1) {
      editHour = abs((int)encoderPos) % 24;
    } else if (editTimeMode == 2) {
      editMinute = abs((int)encoderPos) % 60;
    } else if (editTimeMode == 3) {
      int tz = ((int)encoderPos % 25);
      editTimezone = tz - 12;
    }
    break;
  default:
    break;
  }
  updateDisplay();
}

// ---------------- Navigation ----------------
void slideToMenu(MenuState newMenu) {
  previousMenu = currentMenu;
  currentMenu = newMenu;
  menuPosition = 0;
  resetEncoderPosition(0);
  isSliding = true;
  slideOffset = 0;
  targetOffset = -128;
}

void handleSlideAnimation() {
  if (isSliding) {
    slideOffset -= 12;
    if (slideOffset <= targetOffset) {
      slideOffset = 0;
      isSliding = false;
    }
    updateDisplay();
  }
}

// ---------------- Rendering helpers ----------------
void updateDisplay() {
  display.clearDisplay();
  if (isSliding) {
    drawMenu(previousMenu, slideOffset + 128);
    drawMenu(currentMenu, slideOffset);
  } else {
    drawMenu(currentMenu, 0);
  }
  display.display();
}

void drawMenu(MenuState menu, int offset) {
  switch (menu) {
  case HOME_SCREEN:
    drawHomeScreen(offset);
    break;
  case MAIN_MENU:
    drawMainMenu(offset);
    break;
  case WIFI_MENU:
    drawWiFiMenu(offset);
    break;
  case WIFI_SCAN:
    drawWiFiScanMenu(offset);
    break;
  case WIFI_PASSWORD:
    drawPasswordMenu(offset);
    break;
  case TIME_SETTINGS:
    drawTimeSettings(offset);
    break;
  case WIFI_CONNECTING:
    drawConnectingScreen(offset);
    break;
  case ABOUT_SCREEN:
    drawAboutScreen(offset);
    break;
  case LANGUAGE_MENU:
    drawLanguageMenu(offset);
    break;
  }
}

void drawHomeScreen(int offset) {
  display.setTextSize(2);
  display.setTextColor(WHITE);
  if (!getLocalTime(&timeinfo) && !timeSet) {
    display.setCursor(25 + offset, 8);
    display.println("--:--");
  } else {
    display.setCursor(25 + offset, 8);
    display.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }
  display.setTextSize(1);
  display.setCursor(20 + offset, 28);
  if (timeSet) {
    display.printf("%02d.%02d.%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1,
                   timeinfo.tm_year + 1900);
  } else {
    printText("Нет связи", "Немає зв'язку", "No connection");
  }
  display.drawBitmap(8 + offset, 45, icon_wifi, 16, 16, WHITE);
  display.setCursor(28 + offset, 48);
  if (WiFi.status() == WL_CONNECTED) {
    String ssid = WiFi.SSID();
    if (ssid.length() > 12)
      ssid = ssid.substring(0, 9) + "...";
    display.println(ssid);
  } else {
    printText("Отключен", "Відключено", "Disconnected");
  }
  display.setCursor(100 + offset, 56);
  printText("МЕНЮ", "МЕНЮ", "MENU");
}

void drawMainMenu(int offset) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(10 + offset, 2);
  printText("ГЛАВНОЕ МЕНЮ", "ГОЛОВНЕ МЕНЮ", "MAIN MENU");
  for (int i = 0; i < 3; i++) {
    int y = 14 + i * 12;
    if (i == menuPosition) {
      display.fillRoundRect(2 + offset, y - 1, 124, 10, 1, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }
    display.drawBitmap(4 + offset, y, mainMenuItems[i].icon, 16, 16,
                       i == menuPosition ? BLACK : WHITE);
    display.setCursor(22 + offset, y + 2);
    printText(mainMenuItems[i].title_ru, mainMenuItems[i].title_ua,
              mainMenuItems[i].title_en);
    display.setTextColor(WHITE);
  }
}

void drawWiFiMenu(int offset) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(30 + offset, 2);
  printText("WiFi МЕНЮ", "WiFi МЕНЮ", "WiFi MENU");
  if (menuPosition == 0) {
    display.fillRoundRect(4 + offset, 16, 120, 14, 2, WHITE);
    display.setTextColor(BLACK);
  }
  display.drawBitmap(8 + offset, 18, icon_wifi, 16, 16,
                     menuPosition == 0 ? BLACK : WHITE);
  display.setCursor(28 + offset, 22);
  printText("Поиск сетей", "Пошук мереж", "Scan networks");
  display.setTextColor(WHITE);
  if (menuPosition == 1) {
    display.fillRoundRect(4 + offset, 34, 120, 14, 2, WHITE);
    display.setTextColor(BLACK);
  }
  display.setCursor(28 + offset, 40);
  printText("Назад", "Назад", "Back");
  display.setTextColor(WHITE);
}

void drawWiFiScanMenu(int offset) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(25 + offset, 2);
  printText("СЕТИ WiFi", "МЕРЕЖІ WiFi", "WiFi NETWORKS");
  int startIndex = max(0, menuPosition - 3);
  int endIndex = min(networkCount, startIndex + 4);
  for (int i = startIndex; i < endIndex; i++) {
    int y = 15 + (i - startIndex) * 12;
    if (i == menuPosition) {
      display.fillRoundRect(2 + offset, y - 1, 124, 10, 1, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }
    drawSignalStrength(4 + offset, y, wifiSignals[i], i == menuPosition);
    display.setCursor(20 + offset, y);
    String networkName = wifiNetworks[i];
    if (networkName.length() > 15) {
      networkName = networkName.substring(0, 12) + "...";
    }
    display.println(networkName);
    display.setTextColor(WHITE);
  }
  if (menuPosition >= networkCount) {
    display.fillRoundRect(2 + offset, 52, 124, 10, 1, WHITE);
    display.setTextColor(BLACK);
    display.setCursor(50 + offset, 54);
    printText("Назад", "Назад", "Back");
    display.setTextColor(WHITE);
  }
}

void drawPasswordMenu(int offset) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(2 + offset, 2);
  printText("Пароль:", "Пароль:", "Password:");
  display.setCursor(2 + offset, 12);
  String shortSSID = selectedSSID;
  if (shortSSID.length() > 18) {
    shortSSID = shortSSID.substring(0, 15) + "...";
  }
  display.println(shortSSID);
  display.drawRect(2 + offset, 24, 124, 12, WHITE);
  display.setCursor(4 + offset, 27);
  if (showPassword) {
    display.println(wifiPassword);
  } else {
    for (int i = 0; i < (int)wifiPassword.length(); i++) {
      display.print("*");
    }
  }
  int cursorX = 4 + offset + passwordCursorPos * 6;
  display.drawLine(cursorX, 34, cursorX + 4, 34, WHITE);
  display.setCursor(2 + offset, 42);
  printText("Символ: ", "Символ: ", "Char: ");
  if (passwordCharIndex < charArraySize) {
    display.print(passwordChars[passwordCharIndex]);
  }
  display.setCursor(2 + offset, 52);
  printText("Длин.наж-показать", "Довге натис-показати",
            "Long press-show");
}

void drawTimeSettings(int offset) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(5 + offset, 2);
  printText("НАСТРОЙКИ ВРЕМЕНИ", "НАЛАШТУВАННЯ ЧАСУ", "TIME SETTINGS");
  if (editTimeMode > 0) {
    display.setCursor(2 + offset, 15);
    printText("Режим редактирования:", "Режим редагування:",
              "Edit mode:");
    if (editTimeMode == 1) {
      display.fillRect(20 + offset, 25, 20, 12, WHITE);
      display.setTextColor(BLACK);
    }
    display.setCursor(22 + offset, 28);
    display.printf("%02d", editHour);
    display.setTextColor(WHITE);
    display.setCursor(42 + offset, 28);
    display.print(":");
    if (editTimeMode == 2) {
      display.fillRect(50 + offset, 25, 20, 12, WHITE);
      display.setTextColor(BLACK);
    }
    display.setCursor(52 + offset, 28);
    display.printf("%02d", editMinute);
    display.setTextColor(WHITE);
    display.setCursor(2 + offset, 42);
    printText("Часовой пояс: ", "Часовий пояс: ", "Timezone: ");
    if (editTimeMode == 3) {
      display.fillRect(85 + offset, 40, 35, 12, WHITE);
      display.setTextColor(BLACK);
    }
    display.setCursor(87 + offset, 43);
    display.printf("UTC%+d", editTimezone);
    display.setTextColor(WHITE);
    display.setCursor(2 + offset, 55);
    printText("Длин.наж-сохранить", "Довге натис-зберегти",
              "Long press-save");
  } else {
    display.setTextSize(2);
    display.setCursor(25 + offset, 20);
    if (getLocalTime(&timeinfo)) {
      display.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    } else {
      display.println("--:--");
    }
    display.setTextSize(1);
    display.setCursor(20 + offset, 40);
    display.printf("UTC%+d", timeZoneOffset);
    display.setCursor(2 + offset, 55);
    printText("Длин.наж-править", "Довге натис-змінити",
              "Long press-edit");
  }
}

void drawConnectingScreen(int offset) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(25 + offset, 15);
  printText("Подключение", "Підключення", "Connecting");
  display.setCursor(10 + offset, 28);
  display.println(selectedSSID);
  for (int i = 0; i < 4; i++) {
    if (i <= animFrame) {
      display.fillCircle(40 + i * 12 + offset, 45, 3, WHITE);
    } else {
      display.drawCircle(40 + i * 12 + offset, 45, 3, WHITE);
    }
  }
}

void drawAboutScreen(int offset) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(35 + offset, 2);
  printText("О СИСТЕМЕ", "ПРО СИСТЕМУ", "ABOUT SYSTEM");
  display.setCursor(5 + offset, 18);
  display.println("ESP32 Smart Device");
  display.setCursor(5 + offset, 28);
  display.println("Version: legacy");
  display.setCursor(5 + offset, 38);
  printText("Память: ", "Пам'ять: ", "Memory: ");
  display.print(ESP.getFreeHeap());
  display.println(" bytes");
  display.setCursor(5 + offset, 55);
  printText("Нажмите для выхода", "Натисніть для виходу", "Press to exit");
}

void drawLanguageMenu(int offset) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(30 + offset, 2);
  printText("ЯЗЫК", "МОВА", "LANGUAGE");
  const char *languages[] = {"Русский", "Українська", "English"};
  for (int i = 0; i < 3; i++) {
    int y = 20 + i * 14;
    if (i == menuPosition) {
      display.fillRoundRect(4 + offset, y - 2, 120, 12, 2, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }
    if (i == (int)currentLanguage) {
      display.fillCircle(12 + offset, y + 4, 3, i == menuPosition ? BLACK : WHITE);
    } else {
      display.drawCircle(12 + offset, y + 4, 3, i == menuPosition ? BLACK : WHITE);
    }
    display.setCursor(22 + offset, y + 1);
    if (i < 2) {
      printRussianString(languages[i]);
    } else {
      display.println(languages[i]);
    }
    display.setTextColor(WHITE);
  }
}

void drawSignalStrength(int x, int y, int rssi, bool inverted) {
  int strength = 0;
  if (rssi > -50)
    strength = 4;
  else if (rssi > -60)
    strength = 3;
  else if (rssi > -70)
    strength = 2;
  else if (rssi > -80)
    strength = 1;
  for (int i = 0; i < 4; i++) {
    int barHeight = (i + 1) * 2;
    if (i < strength) {
      display.fillRect(x + i * 3, y + 8 - barHeight, 2, barHeight,
                       inverted ? BLACK : WHITE);
    } else {
      display.drawRect(x + i * 3, y + 8 - barHeight, 2, barHeight,
                       inverted ? BLACK : WHITE);
    }
  }
}

void drawCustomChar(int16_t x, int16_t y, const uint8_t *charData) {
  for (int i = 0; i < 5; i++) {
    uint8_t column = pgm_read_byte(&charData[i]);
    for (int j = 0; j < 8; j++) {
      if (column & (1 << j)) {
        display.drawPixel(x + i, y + j, WHITE);
      }
    }
  }
}

void printRussianChar(uint8_t ch) {
  int16_t x = display.getCursorX();
  int16_t y = display.getCursorY();
  if (ch >= 192 && ch <= 223) {
    drawCustomChar(x, y, russianFont5x8[ch - 192]);
  } else if (ch >= 224 && ch <= 255) {
    drawCustomChar(x, y, russianSmallFont5x8[ch - 224]);
  } else if (ch >= 128 && ch <= 159) {
    drawCustomChar(x, y, russianSmallFont5x8[ch - 128 + 16]);
  } else if (ch >= 160 && ch <= 167) {
    drawCustomChar(x, y, ukrainianFont5x8[ch - 160]);
  } else {
    display.write(ch);
    return;
  }
  display.setCursor(x + 6, y);
}

void printRussianString(const char *str) {
  int len = strlen(str);
  int i = 0;
  while (i < len) {
    unsigned char c = str[i];
    if (c == 0xD0) {
      if (i + 1 < len) {
        i++;
        unsigned char next = str[i];
        if (next >= 0x90 && next <= 0xBF) {
          printRussianChar(next + 102);
        }
      }
    } else if (c == 0xD1) {
      if (i + 1 < len) {
        i++;
        unsigned char next = str[i];
        if (next >= 0x80 && next <= 0x8F) {
          printRussianChar(next + 144);
        }
      }
    } else if (c < 128) {
      display.write(c);
    }
    i++;
  }
}

String getText(const char *ru_text, const char *ua_text, const char *en_text) {
  switch (currentLanguage) {
  case LANG_RUSSIAN:
    return String(ru_text);
  case LANG_UKRAINIAN:
    return String(ua_text);
  case LANG_ENGLISH:
  default:
    return String(en_text);
  }
}

void printText(const char *ru_text, const char *ua_text, const char *en_text) {
  switch (currentLanguage) {
  case LANG_RUSSIAN:
    printRussianString(ru_text);
    break;
  case LANG_UKRAINIAN:
    printRussianString(ua_text);
    break;
  case LANG_ENGLISH:
  default:
    display.print(en_text);
    break;
  }
}

// ---------------- Wi-Fi helpers ----------------
void scanWiFiNetworks() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(25, 25);
  printText("Сканирование...", "Сканування...", "Scanning...");
  display.display();
  networkCount = WiFi.scanNetworks();
  if (networkCount > 15)
    networkCount = 15;
  for (int i = 0; i < networkCount; i++) {
    wifiNetworks[i] = WiFi.SSID(i);
    wifiSignals[i] = WiFi.RSSI(i);
  }
  for (int i = 0; i < networkCount - 1; i++) {
    for (int j = i + 1; j < networkCount; j++) {
      if (wifiSignals[i] < wifiSignals[j]) {
        String tempSSID = wifiNetworks[i];
        int tempRSSI = wifiSignals[i];
        wifiNetworks[i] = wifiNetworks[j];
        wifiSignals[i] = wifiSignals[j];
        wifiNetworks[j] = tempSSID;
        wifiSignals[j] = tempRSSI;
      }
    }
  }
}

void handlePasswordInput() {
  if (passwordCursorPos < (int)wifiPassword.length()) {
    wifiPassword[passwordCursorPos] = passwordChars[passwordCharIndex];
    passwordCursorPos++;
  } else if (passwordCursorPos == (int)wifiPassword.length()) {
    wifiPassword += passwordChars[passwordCharIndex];
    passwordCursorPos++;
  } else {
    connectToWiFi();
  }
  if (passwordCursorPos <= (int)wifiPassword.length()) {
    passwordCharIndex = 0;
    resetEncoderPosition(0);
  }
  updateDisplay();
}

void handleTimeSettings() {
  if (editTimeMode == 0) {
    editTimeMode = 1;
    getLocalTime(&timeinfo);
    editHour = timeinfo.tm_hour;
    editMinute = timeinfo.tm_min;
    resetEncoderPosition(editHour);
  } else if (editTimeMode == 1) {
    editTimeMode = 2;
    resetEncoderPosition(editMinute);
  } else if (editTimeMode == 2) {
    editTimeMode = 3;
    resetEncoderPosition(editTimezone + 12);
  } else {
    editTimeMode = 0;
    saveTimeSettings();
  }
  updateDisplay();
}

void resetEncoderPosition(long newPos) {
  noInterrupts();
  encoderPos = newPos;
  interrupts();
}

void saveTimeSettings() {
  timeZoneOffset = editTimezone;
  preferences.putInt("timezone", timeZoneOffset);
  configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
  if (editTimeMode > 0) {
    struct timeval tv;
    struct tm tm_info;
    getLocalTime(&tm_info);
    tm_info.tm_hour = editHour;
    tm_info.tm_min = editMinute;
    tm_info.tm_sec = 0;
    time_t t = mktime(&tm_info);
    tv.tv_sec = t;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
  }
  timeSet = true;
}

void connectToWiFi() {
  currentMenu = WIFI_CONNECTING;
  updateDisplay();
  WiFi.begin(selectedSSID.c_str(), wifiPassword.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    attempts++;
    if (attempts % 3 == 0) {
      updateDisplay();
    }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  if (WiFi.status() == WL_CONNECTED) {
    preferences.putString("ssid", selectedSSID);
    preferences.putString("password", wifiPassword);
    configTime(timeZoneOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
    timeSet = true;
    display.drawBitmap(56, 8, icon_wifi, 16, 16, WHITE);
    display.setCursor(25, 28);
    printText("Подключено!", "Підключено!", "Connected!");
    display.setCursor(15, 40);
    String ip = WiFi.localIP().toString();
    display.println("IP: " + ip);
    display.display();
    delay(2500);
    slideToMenu(HOME_SCREEN);
  } else {
    display.setCursor(40, 20);
    printText("ОШИБКА", "ПОМИЛКА", "ERROR");
    display.setCursor(15, 35);
    printText("Неверный пароль", "Невірний пароль", "Wrong password");
    display.setCursor(20, 50);
    printText("или нет сигнала", "або немає сигналу", "or no signal");
    display.display();
    delay(2500);
    slideToMenu(WIFI_SCAN);
  }
}
