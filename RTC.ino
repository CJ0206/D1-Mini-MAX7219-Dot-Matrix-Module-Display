// ----------------------------
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <TimeLib.h>
#include <EEPROM.h>
#include <math.h>

// ----------------------------
// Additional Libraries - each one of these will need to be installed.
// ----------------------------

#include <ArduinoJson.h> // Library used for parsing Json from the API responses
#include <MD_MAX72xx.h>  // Library used for interfacing with the 8x8x4 led matrix
#include <MD_Parola.h>   // Library used for displaying info on the matrix
#include <NTPClient.h>   // Library to get the current time
#include <WiFiUdp.h>     // Library to get the time from a udp server

// Search for "Arduino Json" in the Arduino Library manager
// https://github.com/bblanchon/ArduinoJson


// LED MATRIX DISPLAY DEFINITION
#define HARDWARE_TYPE MD_MAX72XX::DR1CR0RR1_HW
#define MAX_DEVICES  8 //8
#define CLK_PIN   D5  // or SCK
#define DATA_PIN  D7  // or MOSI
#define CS_PIN    D6  // or SS

#define WIFI_STATION_RUNNING_OK     5       //seconds
#define WIFI_SSID_LEN_MAX           128      //bytes
#define WIFI_PASSWORD_LEN_MAX       128      //bytes
#define WIFI_AP_SSID         "MatrixDisplay"
#define WIFI_AP_GUIDE_TEXT          "Connect to MatrixDisplay through WiFi... Then go to 192.168.4.1 in a browser"
#define START_MODE_COUNTER_MAX      0x03
#define RUNNING_MODE_WIFI_AP        0x01
#define RUNNING_MODE_WIFI_STATION    0x02

#define EP_CAPACITY                 512     //bytes
#define EP_IDENTIFIER               0xAA
#define EP_IDENTIFIER_ADDR          0x00
#define EP_START_MODE_ADDR          0x01
#define EP_WIFI_SSID_ADDR           0x02
#define EP_WIFI_PASSWORD_ADDR       (EP_WIFI_SSID_ADDR + WIFI_SSID_LEN_MAX)
#define EP_USERNAME_ADDR     (EP_WIFI_PASSWORD_ADDR + WIFI_PASSWORD_LEN_MAX)

#define EP_SETTINGS_IDENTIFIER        0xBB
#define EP_SETTINGS_IDENTIFIER_ADDR   EP_USERNAME_ADDR
#define EP_DISPLAY_SETTINGS_ADDR      (EP_SETTINGS_IDENTIFIER_ADDR + 1)

struct DisplaySettings {
  /*
   * bit 0: display time
   * bit 1: display day
   * bit 2: display date
   * bit 3: clock_format (0=12h, 1=24h)
   * bit 4: display custom_message
   * bit 5: display seconds
   * bit 6: display countdown
   * bit 7: display countup
   */
  uint8_t flags;
  uint16_t duration_time;
  uint16_t duration_day;
  uint16_t duration_date;
  uint16_t duration_custom_message;
  uint16_t duration_countdown;
  uint16_t duration_countup;
  uint16_t fade_time;
  uint8_t scroll_speed;
  uint8_t max_brightness;
  char custom_message[128];
  time_t countdown_target;
  char countdown_complete_message[64];
};

DisplaySettings g_displaySettings;

MD_Parola myDisplay = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
//

// Count-up timer variables
unsigned long g_countup_start_time = 0;
unsigned long g_countup_elapsed_time = 0;
boolean g_countup_running = false;

const int timeDelay = 3000; // Delay between Time / Day / Subs / Views
int statusCode;

String st;
String content;


enum class DisplayState : uint8_t {
  FADE_IN,
  SHOWING,
  SCROLLING,
  FADE_OUT,
  NEXT_ITEM
};

enum class Values : uint8_t {
  curTime = 0,
  weekDay = 1,
  curDate = 2,
  customMessage = 3,
  countdown = 4,
  countUp = 5,
  count = 6
};

typedef enum {
  E_TRANS_TYPE_FADE = (0),
  E_TRANS_TYPE_SCROLL
} e_TransitionType;

typedef enum {
  E_SCROLL_SPEED_SLOW = (0),
  E_SCROLL_SPEED_MEDIUM,
  E_SCROLL_SPEED_FAST
} e_ScrollSpeed;

/* Number of elements mus be equal Values.count */
boolean g_dispState[] = { 
  /* Time */      true,
  /* Day */       false,
  /* Date */      false,
  /* Msg */       false,
  /* Countdown */ false,
  /* CountUp */   false,
};

static int g_dispTextIdx = 0;
DisplayState g_displaySm = DisplayState::FADE_IN;
DisplayState g_lastDisplaySm = DisplayState::NEXT_ITEM;
uint32_t g_lastChangeTime = 0;
int g_fade_intensity = 0;

String feed[(uint8_t)Values::count];

//Time Variables
WiFiUDP ntpUDP; // Define NTP Client to get time
NTPClient timeClient(ntpUDP, "pool.ntp.org");

String weekDays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"}; //Day Names
String myMonthStr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char *AMPM = "AM"; // AM / PM // Variable

//-----------------------------
// For HTTPS requests - keep this
WiFiClientSecure client;

/* Wifi credentials are configurated at runtime and stored on EEPROM */
char ssid[WIFI_SSID_LEN_MAX];
char password[WIFI_PASSWORD_LEN_MAX];

// Web server
ESP8266WebServer apserver(80);

/* Soft AP network parameters */
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);


// Flag to indicate a WiFi connection should be attempted.
boolean connect;

// Timestamp of the last WiFi connection attempt.
unsigned long lastConnectTry = 0;

// Current WLAN status
unsigned int status = WL_IDLE_STATUS;

static uint8_t g_scrollSpeed = E_SCROLL_SPEED_MEDIUM;
static uint8_t g_runningMode = RUNNING_MODE_WIFI_AP;
static boolean g_isWifiStationOk = false;
static String  g_scrollingText = "";

static int g_wifiSsidID = 0;
static int g_networkNum = 0;

void(* SoftReset) (void) = 0;  // declare reset fuction at address 0
void handleWebRoot( void );
void handleSelectNetwork( void );
void handleInputPassword( void );
void handleWifiConnected( void );
void handleConnectWifi( void );
void handleNotFound( void );
void handleSettings( void );
void handleSaveSettings( void );
void initializeDefaultDisplaySettings( void );
void handleConfirmResetPage( void );
void handleFactoryReset( void );
void handleStartTimer( void );
void handleStopTimer( void );
void handleResetTimer( void );
void handleGetTimer( void );


int LocalGetScrollSpeedMs(uint8_t type)
{
  int speed = 100;
  switch (type)
  {
    case E_SCROLL_SPEED_SLOW:   speed = 100; break;
    case E_SCROLL_SPEED_MEDIUM: speed = 50; break;
    case E_SCROLL_SPEED_FAST:   speed = 25; break;
    default: break;
  }

  return speed;
}

// Checks if a given string is a valid IP address.
boolean isIp(String str) {
  for (size_t i = 0; i < str.length(); i++) {
    int c = str.charAt(i);
    if (c != '.' && (c < '0' || c > '9')) {
      return false;
    }
  }
  return true;
}

// Converts an IPAddress object to its string representation.
String toStringIp(IPAddress ip) {
  String res = "";
  for (int i = 0; i < 3; i++) {
    res += String((ip >> (8 * i)) & 0xFF) + ".";
  }
  res += String(((ip >> 8 * 3)) & 0xFF);
  return res;
}

void setup() {
  uint8_t i = 0;
  delay(1000);
  Serial.begin(115200);
  
  //initialize the LED MAtrix 8, 8x8
  myDisplay.begin();
  myDisplay.setZoneEffect(0, true, PA_FLIP_LR);
  myDisplay.setIntensity(0);
  myDisplay.setTextAlignment(PA_CENTER);
  myDisplay.setPause(2000);
  myDisplay.setSpeed(40);
  myDisplay.displayClear();
  myDisplay.print("Loading...");
  
  unsigned long loading_delay = millis();
  while (millis() - loading_delay < 1000) {
    yield();
  }

    Serial.println("Checking EEPROM for settings...");

    EEPROM.begin(EP_CAPACITY);
    uint8_t epId = EEPROM.read(EP_IDENTIFIER_ADDR);

    if (epId != EP_IDENTIFIER)
    {
      // This is a true factory reset or first boot.
      Serial.print("EEPROM has wrong ID: "); Serial.println(epId, DEC);
      EEPROM.write(EP_IDENTIFIER_ADDR, EP_IDENTIFIER);
      EEPROM.write(EP_START_MODE_ADDR, 0x00);
      initializeDefaultDisplaySettings(); // This also commits the main identifier.
      g_runningMode = RUNNING_MODE_WIFI_AP;
    }
    else
    {
      // EEPROM has been initialized before.
      uint8_t startMode = EEPROM.read(EP_START_MODE_ADDR);
      Serial.print("Start mode counter: "); Serial.println(startMode, DEC);
      
      if (startMode > 0) {
        startMode--;
        EEPROM.write(EP_START_MODE_ADDR, startMode);
        EEPROM.commit();
      }
      
      if (startMode > 0) {
        g_runningMode = RUNNING_MODE_WIFI_STATION;
        
        //Read SSID and Password from EEPROM
        memset(ssid, 0, WIFI_SSID_LEN_MAX);
        memset(password, 0, WIFI_PASSWORD_LEN_MAX);
        for (i = 0; i < WIFI_SSID_LEN_MAX - 1; i++)
        {
          ssid[i] = EEPROM.read(EP_WIFI_SSID_ADDR + i);
        }

        for (i = 0; i < WIFI_PASSWORD_LEN_MAX - 1; i++)
        {
          password[i] = EEPROM.read(EP_WIFI_PASSWORD_ADDR + i);
        }

        // Read display settings
        if (EEPROM.read(EP_SETTINGS_IDENTIFIER_ADDR) == EP_SETTINGS_IDENTIFIER)
        {
          Serial.println("Reading display settings from EEPROM...");
          EEPROM.get(EP_DISPLAY_SETTINGS_ADDR, g_displaySettings);
        }
        else
        {
          // This case handles partial corruption where main ID is ok, but settings ID is not.
          initializeDefaultDisplaySettings();
        }
      }
      else {
        //Wifi AP mode
        g_runningMode = RUNNING_MODE_WIFI_AP;
        // When entering AP mode, we should still load the display settings,
        // or initialize them if they are missing.
        if (EEPROM.read(EP_SETTINGS_IDENTIFIER_ADDR) == EP_SETTINGS_IDENTIFIER)
        {
          EEPROM.get(EP_DISPLAY_SETTINGS_ADDR, g_displaySettings);
        }
        else
        {
          initializeDefaultDisplaySettings();
        }
      }
    }

    // This block should always run to ensure the display state reflects the loaded/initialized settings.
    g_dispState[(uint8_t)Values::curTime] = (g_displaySettings.flags & (1 << 0)) != 0;
    g_dispState[(uint8_t)Values::weekDay] = (g_displaySettings.flags & (1 << 1)) != 0;
    g_dispState[(uint8_t)Values::curDate] = (g_displaySettings.flags & (1 << 2)) != 0;
    g_dispState[(uint8_t)Values::customMessage] = (g_displaySettings.flags & (1 << 4)) != 0;
    g_dispState[(uint8_t)Values::countdown] = (g_displaySettings.flags & (1 << 6)) != 0;
    g_dispState[(uint8_t)Values::countUp] = (g_displaySettings.flags & (1 << 7)) != 0;

    // Validate WiFi credentials if in station mode.
    if (g_runningMode == RUNNING_MODE_WIFI_STATION && (String(ssid).length() == 0 || String(password).length() < 8) )
    {
      Serial.print("Invalid WiFi credentials in EEPROM, falling back to AP mode.");
      g_runningMode = RUNNING_MODE_WIFI_AP;
    }


  apserver.on( "/", handleWebRoot );
  apserver.on( "/home", handleWebRoot );
  apserver.on( "/select-network", handleSelectNetwork );
  apserver.on( "/input-password", handleInputPassword );
  apserver.on( "/connect-wifi", handleConnectWifi );
  apserver.on( "/wifi-connected", handleWifiConnected );
  apserver.on( "/settings", handleSettings );
  apserver.on( "/save-settings", handleSaveSettings );
  apserver.on( "/confirm-reset", handleConfirmResetPage );
  apserver.on( "/factory-reset", handleFactoryReset );
  apserver.on( "/start-timer", handleStartTimer );
  apserver.on( "/stop-timer", handleStopTimer );
  apserver.on( "/reset-timer", handleResetTimer );
  apserver.on( "/get-timer", handleGetTimer );
  apserver.onNotFound( handleNotFound );

  if (g_runningMode == RUNNING_MODE_WIFI_AP)
  {
    Serial.println("WIFI Access Point mode");
    WiFi.mode(WIFI_AP);
    boolean conn = WiFi.softAP(WIFI_AP_SSID);

  } else {
    Serial.println("WIFI Client mode");
  }
  apserver.begin();

  i = 0;
  while ( g_dispState[i] == false ) {
    i++;
    if (i == (uint8_t)Values::count) {
      i = 0;
      g_dispState[i] = true;
      break;
    }
  }
  g_dispTextIdx = i;

  //Get the local time
  timeClient.begin(); // Initialize NTPClient --> get time
  timeClient.setTimeOffset(3600);  // Set offset time in seconds to adjust for your timezone: 3600 sec per Hour
  connect = strlen(ssid) > 0; // Request WLAN connect if there is a SSID
  
  if (g_runningMode == RUNNING_MODE_WIFI_AP)
  {
    g_scrollingText = WIFI_AP_GUIDE_TEXT;
    myDisplay.displayScroll(g_scrollingText.c_str(), PA_CENTER, PA_SCROLL_RIGHT, LocalGetScrollSpeedMs(g_scrollSpeed));
  }

}

void connectWifi() {
  Serial.println("Connecting as wifi station...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int connRes = WiFi.waitForConnectResult();
  client.setInsecure();
  Serial.print("connRes: ");
  Serial.println(connRes);
}

void statemachine() {
  const uint8_t FADE_STEPS = 15;
  uint16_t fade_time = g_displaySettings.fade_time;
  if (fade_time == 0) fade_time = 1; // prevent division by zero

  // State entry logic
  if (g_displaySm != g_lastDisplaySm) {
    switch (g_displaySm) {
      case DisplayState::FADE_IN:
      case DisplayState::SHOWING:
      case DisplayState::FADE_OUT:
      case DisplayState::SCROLLING:
        g_lastChangeTime = millis();
        break;
    }
    g_lastDisplaySm = g_displaySm;
  }

  // State machine
  switch (g_displaySm) {
    case DisplayState::FADE_IN:
      if (strlen(feed[g_dispTextIdx].c_str()) > 10) {
        myDisplay.setIntensity(g_displaySettings.max_brightness);
        myDisplay.displayScroll(feed[g_dispTextIdx].c_str(), PA_CENTER, PA_SCROLL_RIGHT, LocalGetScrollSpeedMs(g_displaySettings.scroll_speed));
        g_displaySm = DisplayState::SCROLLING;
      } else {
        myDisplay.print(feed[g_dispTextIdx]);
        if (millis() - g_lastChangeTime > (fade_time / FADE_STEPS)) {
          g_fade_intensity++;
          myDisplay.setIntensity( map(g_fade_intensity, 0, 15, 0, g_displaySettings.max_brightness) );
          g_lastChangeTime = millis();
          if (g_fade_intensity >= FADE_STEPS) {
            g_fade_intensity = FADE_STEPS;
            g_displaySm = DisplayState::SHOWING;
          }
        }
      }
      break;

    case DisplayState::SHOWING:
    {
      if (g_dispTextIdx == (uint8_t)Values::curTime || g_dispTextIdx == (uint8_t)Values::countdown || g_dispTextIdx == (uint8_t)Values::countUp) {
        myDisplay.print(feed[g_dispTextIdx]);
      }
      uint16_t display_time = 0;
      switch (g_dispTextIdx)
      {
        case (uint8_t)Values::curTime: display_time = g_displaySettings.duration_time; break;
        case (uint8_t)Values::weekDay: display_time = g_displaySettings.duration_day; break;
        case (uint8_t)Values::curDate: display_time = g_displaySettings.duration_date; break;
        case (uint8_t)Values::customMessage: display_time = g_displaySettings.duration_custom_message; break;
        case (uint8_t)Values::countdown: display_time = g_displaySettings.duration_countdown; break;
        case (uint8_t)Values::countUp: display_time = g_displaySettings.duration_countup; break;
      }
      if (millis() - g_lastChangeTime > display_time) {
        g_displaySm = DisplayState::FADE_OUT;
      }
      break;
    }

    case DisplayState::SCROLLING:
      if (myDisplay.displayAnimate()) {
        myDisplay.displayReset();
        g_displaySm = DisplayState::FADE_OUT;
      }
      break;

    case DisplayState::FADE_OUT:
      if (strlen(feed[g_dispTextIdx].c_str()) <= 10) {
        myDisplay.print(feed[g_dispTextIdx]);
        if (millis() - g_lastChangeTime > (fade_time / FADE_STEPS)) {
          g_fade_intensity--;
          myDisplay.setIntensity( map(g_fade_intensity, 0, 15, 0, g_displaySettings.max_brightness) );
          g_lastChangeTime = millis();
          if (g_fade_intensity <= 0) {
            g_fade_intensity = 0;
            g_displaySm = DisplayState::NEXT_ITEM;
          }
        }
      } else {
        g_displaySm = DisplayState::NEXT_ITEM;
      }
      break;

    case DisplayState::NEXT_ITEM:
      g_dispTextIdx++;
      if (g_dispTextIdx >= (uint8_t)Values::count) {
        g_dispTextIdx = 0;
      }
      
      int loop_guard = 0;
      while (g_dispState[g_dispTextIdx] == false && loop_guard < (int)Values::count) {
        g_dispTextIdx++;
        loop_guard++;
        if (g_dispTextIdx >= (uint8_t)Values::count) {
          g_dispTextIdx = 0;
        }
      }
      
      myDisplay.displayClear();
      g_displaySm = DisplayState::FADE_IN;
      break;
  }
}

void loop() {
  uint32_t currentTime = millis();

  if (g_countup_running) {
    g_countup_elapsed_time = millis() - g_countup_start_time;
  }

  if ( g_runningMode == RUNNING_MODE_WIFI_STATION )
  {
    if ( g_isWifiStationOk == false )
    {
      if (millis() > WIFI_STATION_RUNNING_OK*1000)
      {
        g_isWifiStationOk = true;
        EEPROM.write(EP_START_MODE_ADDR, START_MODE_COUNTER_MAX);
        EEPROM.commit();
      }
    }
      
    if (connect) {
      Serial.println("Connect requested");
      connect = false;
      connectWifi();
      lastConnectTry = millis();
    }

    unsigned int s = WiFi.status();
    if (s == 0 && millis() > (lastConnectTry + 60000)) {
      /* If WLAN disconnected and idle try to connect */
      /* Don't set retry time too low as retry interfere the softAP operation */
      connect = true;
    }

    if (status != s) { // WLAN status change
      Serial.print("Status: ");
      Serial.println(s);
      status = s;
      if (s == WL_CONNECTED) {
        /* Just connected to WLAN */
        Serial.println("");
        Serial.print("Connected to ");
        Serial.println(ssid);
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

      } else if (s == WL_NO_SSID_AVAIL) {
        WiFi.disconnect();
      }
    }
    if (s == WL_CONNECTED) {
      timeClient.update();
    }
    else {
    }

    // Do work:
    configureData();

    uint8_t enabled_modes = 0;
    for (int i=0; i < (int)Values::count; i++) {
      if (g_dispState[i]) enabled_modes++;
    }

    if (enabled_modes <= 1) {
      int single_mode_idx = -1;
      for (int i=0; i < (int)Values::count; i++) { if (g_dispState[i]) { single_mode_idx = i; break; } }

      if (single_mode_idx != -1) {
        bool isLongMessage = strlen(feed[single_mode_idx].c_str()) > 10;
        
        myDisplay.setIntensity(g_displaySettings.max_brightness);

        if (isLongMessage) {
          if (myDisplay.displayAnimate()) {
            myDisplay.displayScroll(feed[single_mode_idx].c_str(), PA_CENTER, PA_SCROLL_RIGHT, LocalGetScrollSpeedMs(g_displaySettings.scroll_speed));
          }
        } else {
          myDisplay.print(feed[single_mode_idx]);
        }
      } else {
        myDisplay.displayClear();
      }
    } else {
      statemachine();
    }
  }
  else  //WIFI Access Point mode
  {
    if (myDisplay.displayAnimate())
    {
      myDisplay.displayReset();
      myDisplay.displayScroll(g_scrollingText.c_str(), PA_CENTER, PA_SCROLL_RIGHT, LocalGetScrollSpeedMs(g_scrollSpeed));
    }
  }
  apserver.handleClient();
}


void configureData() {
  timeClient.update();

  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  int currentSecond = timeClient.getSeconds();
  int epochTime = timeClient.getEpochTime();
  
  char time[40];

  if (g_displaySettings.flags & (1 << 3)) // 24hr format
  {
    if (g_displaySettings.flags & (1 << 5)) { // show seconds
      sprintf(time, "%02d:%02d:%02d", currentHour, currentMinute, currentSecond);
    } else {
      sprintf(time, "%02d:%02d", currentHour, currentMinute);
    }
  }
  else // 12hr format
  {
    //AM or PM adjustment
    if (currentHour >= 12)
    {
      AMPM = "PM";
    }
    else(AMPM = "AM");
    if (currentHour >= 13) currentHour = currentHour - 12;
    if (currentHour == 0) currentHour = 12;

    if (g_displaySettings.flags & (1 << 5)) { // show seconds
      sprintf(time, "%d:%02d:%02d %s", currentHour, currentMinute, currentSecond, AMPM);
    } else {
      sprintf(time, "%d:%02d %s", currentHour, currentMinute, AMPM);
    }
  }

  String weekDay = weekDays[timeClient.getDay()];
  //  Serial.println(weekDay);

  char dateStr[32];
  sprintf( dateStr, "%s %d, %d", myMonthStr[month(epochTime) - 1].c_str(), day(epochTime), year(epochTime) );

// 
  myDisplay.setTextAlignment(PA_CENTER);

  feed[(uint8_t)Values::curTime] = time;
  feed[(uint8_t)Values::weekDay] = weekDay;
  feed[(uint8_t)Values::curDate] = dateStr;
  feed[(uint8_t)Values::customMessage] = g_displaySettings.custom_message;

  if (g_displaySettings.flags & (1 << 6)) {
    time_t now = timeClient.getEpochTime();
    long diff = g_displaySettings.countdown_target - now;
    char countdownStr[32];

    if (diff > 0) {
      int days = diff / 86400;
      diff %= 86400;
      int hours = diff / 3600;
      diff %= 3600;
      int minutes = diff / 60;
      int seconds = diff % 60;

      if (days > 0) {
        sprintf(countdownStr, "%dd %dh", days, hours);
      } else if (hours > 0) {
        sprintf(countdownStr, "%dh %dm", hours, minutes);
      } else if (minutes > 0) {
        sprintf(countdownStr, "%dm %ds", minutes, seconds);
      } else {
        sprintf(countdownStr, "%ds", seconds);
      }
    } else {
      if (strlen(g_displaySettings.countdown_complete_message) > 0) {
        strcpy(countdownStr, g_displaySettings.countdown_complete_message);
      } else {
        strcpy(countdownStr, "Time's Up!");
      }
    }
    feed[(uint8_t)Values::countdown] = countdownStr;
  } else {
    feed[(uint8_t)Values::countdown] = "";
  }

  if (g_displaySettings.flags & (1 << 7)) {
    unsigned long total_seconds = g_countup_elapsed_time / 1000;
    unsigned long hours = total_seconds / 3600;
    unsigned long minutes = (total_seconds % 3600) / 60;
    unsigned long seconds = total_seconds % 60;
    char time_buffer[10];
    sprintf(time_buffer, "%02lu:%02lu:%02lu", hours, minutes, seconds);
    feed[(uint8_t)Values::countUp] = time_buffer;
  } else {
    feed[(uint8_t)Values::countUp] = "";
  }
}


static const char* PROGMEM imgWifi01 = "";
static const char* PROGMEM imgWifi02 = "";
static const char* PROGMEM imgWifi03 = "";
static const char* PROGMEM imgWifi04 = "";
static const char* PROGMEM respTemplate = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                  "<!DOCTYPE html>"
                  "<html>"
                  "<head>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1.0'/>"
                  "<meta charset='utf-8'>"
                  "<style>{{style}}</style>"
                  "<title>CJs Matrix</title>"
                  "</head>"
                  "<body>{{body}}<script>{{script}}</script></body></html>";
static const char* PROGMEM respStyle = "body { background-color: #121212; color: #e0e0e0; font-family: Arial, sans-serif; font-size: 110%; margin: 0; padding: 20px; } #main { max-width: 500px; margin: auto; padding: 20px; background-color: #1e1e1e; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); } h2 { text-align: center; color: #ffffff; } .button, .active-btn, .continue-btn, .yes-btn, .no-btn, .reset-btn { padding: 12px 20px; width: 100%; font-size: 120%; cursor: pointer; border-radius: 5px; border: none; color: white; margin-top: 10px; box-sizing: border-box; } .active-btn { background-color: #007bff; } .active-btn:hover { background-color: #0056b3; } .yes-btn { background-color: #28a745; } .yes-btn:hover { background-color: #218838; } .no-btn { background-color: #dc3545; } .no-btn:hover { background-color: #c82333; } .reset-btn { background-color: #dc3545; } .reset-btn:hover { background-color: #c82333; } .continue-btn { background-color: #6c757d; } .continue-btn:hover { background-color: #5a6268; } .main-page-url { font-size: 140%; text-decoration: underline; color: #007bff; } .large-text { font-size: 120%; } .center { padding-top: 0px; width: 100%; display: flex; flex-direction: column; align-items: center; text-align: center; } .center > div { margin: 5px; } .center-container { width: 100%; display: flex; flex-direction: column; align-items: center; } .center-children { width: 100%; } .center-children > form input[type='text'], .center-children > form input[type='password'], .center-children > form input[type='number'], .center-children > form input[type='datetime-local'], .center-children > form textarea, .center-children > form select { padding: 10px; width: 100%; border-radius: 5px; border: 1px solid #444; background-color: #333; color: #e0e0e0; box-sizing: border-box; margin-top: 5px; } .center-children > form .input-label { margin-top: 15px; margin-bottom: 5px; color: #aaa; display: block; } .center-children > form .btn-submit { width: 100%; display: flex; justify-content: center; margin-top: 20px; } .flex-container { display: flex; justify-content: center; margin-bottom: 10px; } .header { margin: 20px 0; } .table-container { width: 100%; border-collapse: collapse; } .table-container tr { border-bottom: 1px solid #444; } .table-container tr:last-child { border-bottom: none; } .table-container td { padding: 10px; } .table-underline { text-decoration: none; color: #007bff; cursor: pointer; } .table-underline:hover { text-decoration: underline; } .wifi-level { width: 20px; height: auto; } a { color: #007bff; text-decoration: none; } a:hover { text-decoration: underline; } .switch { position: relative; display: inline-block; width: 50px; height: 28px; } .switch input { opacity: 0; width: 0; height: 0; } .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #dc3545; -webkit-transition: .4s; transition: .4s; border-radius: 28px; } .slider:before { position: absolute; content: ''; height: 20px; width: 20px; left: 4px; bottom: 4px; background-color: white; -webkit-transition: .4s; transition: .4s; border-radius: 50%; } input:checked + .slider { background-color: #28a745; } input:checked + .slider:before { -webkit-transform: translateX(22px); -ms-transform: translateX(22px); transform: translateX(22px); } label { vertical-align: middle; } .setting-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; } .module-section { border: 1px solid #444; border-radius: 8px; padding: 15px; margin: 15px 0; } .module-title { font-size: 1.1em; color: #ffffff; margin-bottom: 10px; }";

static const char* PROGMEM webRoot = "<div id='main'> <div class='flex-container'> </div> <h2 class='header'>CJs Matrix</h2> <div class='center'> <div>Welcome to the Matrix Display setup.</div> </div> <div class='center'> <a href='select-network'><input class='button active-btn' type='submit' value='Setup WiFi'></input></a> </div> <div class='center'> <a href='settings'><input class='button active-btn' type='submit' value='Settings' /></a> </div> <div class='center'> <a href='confirm-reset'><input class='button reset-btn' type='submit' value='Factory Reset' /></a> </div></div>";

static const char* PROGMEM webOptionNo = "<div id='main'> <div class='flex-container'> </div> <h2 class='header'>CJs Matrix</h2> <div class='center'> <div>This option is no longer available.</div> </div> <div class='center'> <a href='home'><input class='button active-btn' type='submit' value='CONTINUE' /></a> </div> </div>";

static const char* PROGMEM webSelectNetwork = "<div id='main'><div class='flex-container'></div><h2 class='header'>CJs Matrix</h2>{{networks}}</div>";

static const char* PROGMEM webInputPassword = " <div id='main'> <div class='flex-container'> </div> <h2 class='header'>CJs Matrix</h2> <div class='center-container'> <div class='center-children'> <form action='connect-wifi' method='get'> <div class='input-label'>NETWORK PASSWORD</div> <input type='password' placeholder='Password' id='password' name='password' /> <div class='btn-submit'> <input class='button active-btn' type='submit' value='CONNECT' id='btnConnect'/> </div> </form> </div> </div> </div>";

static const char* PROGMEM webWifiConnected = "<div id='main'> <div class='flex-container'>      </div> <h2 class='header'>CJs Matrix</h2> <div class='center'> <div>Your Matrix Display will now reboot and connect to the selected WiFi network.</div> <div>You can adjust settings by navigating to the device's IP address in a browser.</div> </div> <script> </script> </div>";

static const char* PROGMEM webSettingsPage = "<div id='main'> <div class='flex-container'> </div> <h2 class='header'>Display Settings</h2> <div class='center-container'> <div class='center-children'> <form action='save-settings' method='get'> <div class='module-section'> <div class='module-title'>Time</div> <div class='setting-row'> <span>Display Time</span> <label class='switch'><input type='checkbox' id='time' name='time' value='1' {{time_checked}}><span class='slider'></span></label> </div> <label for='duration_time'>Duration (ms): (min 5000)</label><input type='number' id='duration_time' name='duration_time' value='{{duration_time}}'><br> <input type='checkbox' id='show_seconds' name='show_seconds' value='1' {{show_seconds_checked}}><label for='show_seconds'>Show Seconds</label><br> <select name='clock_format' id='clock_format'><option value='12' {{12hr_selected}}>12 Hour</option><option value='24' {{24hr_selected}}>24 Hour</option></select> </div> <div class='module-section'> <div class='module-title'>Day of Week</div> <div class='setting-row'> <span>Display Day of Week</span> <label class='switch'><input type='checkbox' id='day' name='day' value='1' {{day_checked}}><span class='slider'></span></label> </div> <label for='duration_day'>Duration (ms): (min 5000)</label><input type='number' id='duration_day' name='duration_day' value='{{duration_day}}'><br> </div> <div class='module-section'> <div class='module-title'>Date</div> <div class='setting-row'> <span>Display Date</span> <label class='switch'><input type='checkbox' id='date' name='date' value='1' {{date_checked}}><span class='slider'></span></label> </div> <label for='duration_date'>Duration (ms): (min 5000)</label><input type='number' id='duration_date' name='duration_date' value='{{duration_date}}'><br> </div> <div class='module-section'> <div class='module-title'>Custom Message</div> <div class='setting-row'> <span>Display Custom Message</span> <label class='switch'><input type='checkbox' id='custom_message_cb' name='custom_message_cb' value='1' {{custom_message_checked}}><span class='slider'></span></label> </div> <label for='duration_custom_message'>Duration (ms): (min 5000)</label><input type='number' id='duration_custom_message' name='duration_custom_message' value='{{duration_custom_message}}'><br> <div class='input-label'>Predefined Messages</div><select id='predefined_messages' onchange='updateCustomMessage()'><option value=''>-- Select a message --</option><option value='BUSY'>⛔ BUSY</option><option value='ON A CALL'>📞 ON A CALL</option><option value='ON AIR'>🔴 ON AIR</option><option value='FREE'>☕ FREE</option><option value='AWAY'>🚪 AWAY</option><option value='WAIT'>✋🏻 WAIT</option></select><br><div class='input-label'>Custom Message</div> <textarea id='custom_message_text' name='custom_message' rows='3' style='width: 100%'>{{custom_message}}</textarea> </div> <div class='module-section'> <div class='module-title'>Countdown</div> <div class='setting-row'> <span>Display Countdown</span> <label class='switch'><input type='checkbox' id='countdown' name='countdown' value='1' {{countdown_checked}}><span class='slider'></span></label> </div> <label for='duration_countdown'>Duration (ms): (min 5000)</label><input type='number' id='duration_countdown' name='duration_countdown' value='{{duration_countdown}}'><br> <div class='input-label'>Countdown Target</div><input type='datetime-local' name='countdown_target_dt' value='{{countdown_target_dt}}'><br> <div class='input-label'>Countdown Complete Message</div> <input type='text' id='countdown_complete_message' name='countdown_complete_message' value='{{countdown_complete_message}}' maxlength='64'> </div> <div class='module-section'> <div class='module-title'>Count-Up Timer</div> <div class='setting-row'> <span>Display Count-Up Timer</span> <label class='switch'><input type='checkbox' id='countup' name='countup' value='1' {{countup_checked}}><span class='slider'></span></label> </div> <label for='duration_countup'>Duration (ms): (min 5000)</label><input type='number' id='duration_countup' name='duration_countup' value='{{duration_countup}}'><br> <div class='flex-container' style='justify-content: space-around; align-items: center; padding: 10px; border: 1px solid #444; border-radius: 5px; margin-top: 10px;'> <span id='countup_timer_display' style='font-size: 1.2em; color: #e0e0e0; font-family: monospace;'>00:00:00.0</span> <button type='button' class='button active-btn' style='width: auto; margin: 0 5px;' onclick='startCountup()'>Start</button> <button type='button' class='button continue-btn' style='width: auto; margin: 0 5px;' onclick='stopCountup()'>Stop</button> <button type='button' class='button reset-btn' style='width: auto; margin: 0 5px;' onclick='resetCountup()'>Reset</button> </div> </div> <div class='module-section'> <div class='module-title'>Display Settings</div> <label for='scroll_speed'>Scroll Speed</label> <select name='scroll_speed' id='scroll_speed'> <option value='0' {{scroll_slow_selected}}>Slow</option> <option value='1' {{scroll_medium_selected}}>Medium</option> <option value='2' {{scroll_fast_selected}}>Fast</option> </select><br> <label for='max_brightness'>Max Brightness</label> <input type='range' min='0' max='15' value='{{max_brightness}}' name='max_brightness' /><br> <label for='fade_time'>Fade Time (ms): (min 200)</label><input type='number' id='fade_time' name='fade_time' value='{{fade_time}}'><br> </div> <div class='btn-submit'> <input class='button active-btn' type='submit' value='SAVE SETTINGS' /> </div> </form> </div> </div> </div><script>function updateCustomMessage() { var dropdown = document.getElementById('predefined_messages'); var customMessageText = document.getElementById('custom_message_text'); var selectedValue = dropdown.options[dropdown.selectedIndex].value; if (selectedValue) { customMessageText.value = selectedValue; } } let countup_timer_interval; const countup_timer_display = document.getElementById('countup_timer_display'); function formatTime(ms) { const total_seconds = Math.floor(ms / 1000); const hours = Math.floor(total_seconds / 3600); const minutes = Math.floor((total_seconds % 3600) / 60); const seconds = total_seconds % 60; const tenths = Math.floor((ms % 1000) / 100); return `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}.${tenths}`; } function startCountup() { fetch('/start-timer'); } function stopCountup() { fetch('/stop-timer'); } function resetCountup() { fetch('/reset-timer'); } function getTimer() { fetch('/get-timer').then(response => response.json()).then(data => { if(countup_timer_display) { countup_timer_display.textContent = formatTime(data.time); } }); } document.addEventListener('DOMContentLoaded', function() { if(document.getElementById('countup_timer_display')) { getTimer(); countup_timer_interval = setInterval(getTimer, 100); } });</script>";

static const char* PROGMEM webConfirmResetPage = "<div id='main'> <div class='flex-container'> </div> <h2 class='header'>Confirm Reset</h2> <div class='center'> <div>Are you sure you want to proceed?</div> <div>This will clear all saved WiFi credentials and settings. This action cannot be undone.</div> </div> <div class='center'> <a href='factory-reset'><input class='button reset-btn' type='submit' value='Confirm Factory Reset'></input></a> </div> <div class='center'> <a href='/'><input class='button continue-btn' type='submit' value='Cancel' /></a> </div></div>";

static const char* PROGMEM jsSelectNetwork = "function getId(element) {window.location.href='input-password?id=' + element.closest('tr').rowIndex;};";

static const char* PROGMEM jsInputPassword = "";

#define GET_WIFI_LEVEL_IMG(lrssi) \
( (-30 <= lrssi && lrssi <= 0)? FPSTR(imgWifi01) : \
(-67 <= lrssi && lrssi <= -30)? FPSTR(imgWifi02) : \
(-70 <= lrssi && lrssi <= -67)? FPSTR(imgWifi03) : FPSTR(imgWifi04) )

void handleSettings( void )
{
  String response = FPSTR(respTemplate);
  String settingsPage = FPSTR(webSettingsPage);

  settingsPage.replace("{{time_checked}}", (g_displaySettings.flags & (1 << 0)) ? "checked" : "");
  settingsPage.replace("{{day_checked}}", (g_displaySettings.flags & (1 << 1)) ? "checked" : "");
  settingsPage.replace("{{date_checked}}", (g_displaySettings.flags & (1 << 2)) ? "checked" : "");
  settingsPage.replace("{{custom_message_checked}}", (g_displaySettings.flags & (1 << 4)) ? "checked" : "");
  settingsPage.replace("{{countdown_checked}}", (g_displaySettings.flags & (1 << 6)) ? "checked" : "");
  settingsPage.replace("{{countup_checked}}", (g_displaySettings.flags & (1 << 7)) ? "checked" : "");
  settingsPage.replace("{{duration_time}}", String(g_displaySettings.duration_time));
  settingsPage.replace("{{duration_day}}", String(g_displaySettings.duration_day));
  settingsPage.replace("{{duration_date}}", String(g_displaySettings.duration_date));
  settingsPage.replace("{{duration_custom_message}}", String(g_displaySettings.duration_custom_message));
  settingsPage.replace("{{duration_countdown}}", String(g_displaySettings.duration_countdown));
  settingsPage.replace("{{duration_countup}}", String(g_displaySettings.duration_countup));
  settingsPage.replace("{{fade_time}}", String(g_displaySettings.fade_time));
  if (g_displaySettings.countdown_target > 0) {
    char dt_buffer[20];
    sprintf(dt_buffer, "%04d-%02d-%02dT%02d:%02d", 
            year(g_displaySettings.countdown_target), 
            month(g_displaySettings.countdown_target), 
            day(g_displaySettings.countdown_target), 
            hour(g_displaySettings.countdown_target), 
            minute(g_displaySettings.countdown_target));
    settingsPage.replace("{{countdown_target_dt}}", dt_buffer);
  } else {
    settingsPage.replace("{{countdown_target_dt}}", "");
  }
  settingsPage.replace("{{custom_message}}", String(g_displaySettings.custom_message));
  settingsPage.replace("{{countdown_complete_message}}", String(g_displaySettings.countdown_complete_message));
  settingsPage.replace("{{scroll_slow_selected}}", (g_displaySettings.scroll_speed == E_SCROLL_SPEED_SLOW) ? "selected" : "");
  settingsPage.replace("{{scroll_medium_selected}}", (g_displaySettings.scroll_speed == E_SCROLL_SPEED_MEDIUM) ? "selected" : "");
  settingsPage.replace("{{scroll_fast_selected}}", (g_displaySettings.scroll_speed == E_SCROLL_SPEED_FAST) ? "selected" : "");
  settingsPage.replace("{{max_brightness}}", String(g_displaySettings.max_brightness));
  settingsPage.replace("{{show_seconds_checked}}", (g_displaySettings.flags & (1 << 5)) ? "checked" : "");
  settingsPage.replace("{{12hr_selected}}", (g_displaySettings.flags & (1 << 3)) ? "" : "selected");
  settingsPage.replace("{{24hr_selected}}", (g_displaySettings.flags & (1 << 3)) ? "selected" : "");
  
  response.replace("{{style}}", FPSTR(respStyle));
  response.replace("{{body}}", settingsPage);
  response.replace("{{script}}", "");

  apserver.sendContent( response );
}

void handleSaveSettings( void )
{
  Serial.println("Saving settings:");
  g_displaySettings.flags = 0;
  if (apserver.arg("time") == "1") g_displaySettings.flags |= (1 << 0);
  if (apserver.arg("day") == "1") g_displaySettings.flags |= (1 << 1);
  if (apserver.arg("date") == "1") g_displaySettings.flags |= (1 << 2);
  if (apserver.arg("clock_format") == "24") g_displaySettings.flags |= (1 << 3);
  if (apserver.arg("custom_message_cb") == "1") g_displaySettings.flags |= (1 << 4);
  if (apserver.arg("show_seconds") == "1") g_displaySettings.flags |= (1 << 5);
  if (apserver.arg("countdown") == "1") g_displaySettings.flags |= (1 << 6);
  if (apserver.arg("countup") == "1") g_displaySettings.flags |= (1 << 7);
  Serial.print("  flags: "); Serial.println(g_displaySettings.flags, BIN);

  g_displaySettings.duration_time = apserver.arg("duration_time").toInt();
  if (g_displaySettings.duration_time < 5000) g_displaySettings.duration_time = 5000;
  Serial.print("  duration_time: "); Serial.println(g_displaySettings.duration_time);
  g_displaySettings.duration_day = apserver.arg("duration_day").toInt();
  if (g_displaySettings.duration_day < 5000) g_displaySettings.duration_day = 5000;
  Serial.print("  duration_day: "); Serial.println(g_displaySettings.duration_day);
  g_displaySettings.duration_date = apserver.arg("duration_date").toInt();
  if (g_displaySettings.duration_date < 5000) g_displaySettings.duration_date = 5000;
  Serial.print("  duration_date: "); Serial.println(g_displaySettings.duration_date);
  g_displaySettings.duration_custom_message = apserver.arg("duration_custom_message").toInt();
  if (g_displaySettings.duration_custom_message < 5000) g_displaySettings.duration_custom_message = 5000;
  Serial.print("  duration_custom_message: "); Serial.println(g_displaySettings.duration_custom_message);
  g_displaySettings.duration_countdown = apserver.arg("duration_countdown").toInt();
  if (g_displaySettings.duration_countdown < 5000) g_displaySettings.duration_countdown = 5000;
  Serial.print("  duration_countdown: "); Serial.println(g_displaySettings.duration_countdown);
  g_displaySettings.duration_countup = apserver.arg("duration_countup").toInt();
  if (g_displaySettings.duration_countup < 5000) g_displaySettings.duration_countup = 5000;
  Serial.print("  duration_countup: "); Serial.println(g_displaySettings.duration_countup);
  g_displaySettings.fade_time = apserver.arg("fade_time").toInt();
  if (g_displaySettings.fade_time < 200) g_displaySettings.fade_time = 200;
  Serial.print("  fade_time: "); Serial.println(g_displaySettings.fade_time);
  apserver.arg("custom_message").toCharArray(g_displaySettings.custom_message, 128);
  Serial.print("  custom_message: "); Serial.println(g_displaySettings.custom_message);
  apserver.arg("countdown_complete_message").toCharArray(g_displaySettings.countdown_complete_message, 64);
  Serial.print("  countdown_complete_message: "); Serial.println(g_displaySettings.countdown_complete_message);
  g_displaySettings.scroll_speed = apserver.arg("scroll_speed").toInt();
  Serial.print("  scroll_speed: "); Serial.println(g_displaySettings.scroll_speed);
  g_displaySettings.max_brightness = apserver.arg("max_brightness").toInt();
  Serial.print("  max_brightness: "); Serial.println(g_displaySettings.max_brightness);

  String dt_str = apserver.arg("countdown_target_dt");
  tmElements_t tm;
  int year, month, day, hour, minute;
  
  if (sscanf(dt_str.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) == 5) {
    tm.Year = year - 1970;
    tm.Month = month;
    tm.Day = day;
    tm.Hour = hour;
    tm.Minute = minute;
    tm.Second = 0;
    g_displaySettings.countdown_target = makeTime(tm);
  } else {
    g_displaySettings.countdown_target = 0;
  }
  Serial.print("  countdown_target: "); Serial.println(g_displaySettings.countdown_target);

  EEPROM.write(EP_SETTINGS_IDENTIFIER_ADDR, EP_SETTINGS_IDENTIFIER);
  EEPROM.put(EP_DISPLAY_SETTINGS_ADDR, g_displaySettings);
  EEPROM.commit();

  g_dispState[(uint8_t)Values::curTime] = (g_displaySettings.flags & (1 << 0)) != 0;
  g_dispState[(uint8_t)Values::weekDay] = (g_displaySettings.flags & (1 << 1)) != 0;
  g_dispState[(uint8_t)Values::curDate] = (g_displaySettings.flags & (1 << 2)) != 0;
  g_dispState[(uint8_t)Values::customMessage] = (g_displaySettings.flags & (1 << 4)) != 0;
  g_dispState[(uint8_t)Values::countdown] = (g_displaySettings.flags & (1 << 6)) != 0;
  g_dispState[(uint8_t)Values::countUp] = (g_displaySettings.flags & (1 << 7)) != 0;
  
  Serial.println("g_dispState updated:");
  Serial.print("  Time: "); Serial.println(g_dispState[0]);
  Serial.print("  Day: "); Serial.println(g_dispState[1]);
  Serial.print("  Date: "); Serial.println(g_dispState[2]);

  apserver.sendHeader("Location", "/settings", true);
  apserver.send(302, "text/plain", "");
}

void initializeDefaultDisplaySettings() {
  Serial.println("Initializing default display settings...");
  g_displaySettings.flags = (1 << 0); // Display time
  g_displaySettings.duration_time = 5000;
  g_displaySettings.duration_day = 5000;
  g_displaySettings.duration_date = 5000;
  g_displaySettings.duration_custom_message = 5000;
  g_displaySettings.duration_countdown = 5000;
  g_displaySettings.duration_countup = 5000;
  g_displaySettings.countdown_target = 5000;
  g_displaySettings.fade_time = 200;
  g_displaySettings.scroll_speed = E_SCROLL_SPEED_MEDIUM;
  g_displaySettings.max_brightness = 15;
  strcpy(g_displaySettings.custom_message, "Hello World!");
  strcpy(g_displaySettings.countdown_complete_message, "Time's Up!");
  EEPROM.write(EP_SETTINGS_IDENTIFIER_ADDR, EP_SETTINGS_IDENTIFIER);
  EEPROM.put(EP_DISPLAY_SETTINGS_ADDR, g_displaySettings);
  EEPROM.commit();
}

void handleConfirmResetPage( void )
{
  String response = FPSTR(respTemplate);
  
  response.replace("{{style}}", FPSTR(respStyle));
  response.replace("{{body}}", FPSTR(webConfirmResetPage));
  response.replace("{{script}}", "");

  apserver.sendContent( response );
}

void handleFactoryReset( void )
{
  Serial.println("Performing factory reset...");
  // Clear the identifier bytes in EEPROM. This will cause the device
  // to revert to default settings on the next boot.
  EEPROM.write(EP_IDENTIFIER_ADDR, 0x00);
  EEPROM.write(EP_SETTINGS_IDENTIFIER_ADDR, 0x00);
  EEPROM.commit();

  String response = FPSTR(respTemplate);
  String body = "<div id='main'> <div class='flex-container'> </div> <h2 class='header'>Reset Complete</h2> <div class='center'> <div>Device has been reset to factory defaults.</div> <div>It will now reboot.</div> </div></div>";
  response.replace("{{style}}", FPSTR(respStyle));
  response.replace("{{body}}", body);
  response.replace("{{script}}", "<script>setTimeout(function(){ window.location.href = '/'; }, 5000);</script>");
  
  apserver.sendContent( response );

  delay(5000);
  ESP.restart();
}

void handleNotFound( void )
{
  apserver.send(404, "text/plain", "Page Not found");
}

void handleWebRoot( void )
{
  String response = FPSTR(respTemplate);
  
  response.replace("{{style}}", FPSTR(respStyle));
  response.replace("{{body}}", FPSTR(webRoot));
  response.replace("{{script}}", "");

  apserver.sendContent( response );
}


void handleInputPassword( void )
{
  String response = FPSTR(respTemplate);

  if (apserver.hasArg("id"))
  {
    g_wifiSsidID = apserver.arg("id").toInt() - 1;
    Serial.print("Selected network index: "); Serial.println(g_wifiSsidID, DEC);
    response.replace("{{style}}", FPSTR(respStyle));
    response.replace("{{body}}", FPSTR(webInputPassword));
    response.replace("{{script}}", FPSTR(jsInputPassword));
    apserver.sendContent( response );
  }
  else {
    handleNotFound();
  }
}

void handleStartTimer() {
  if (!g_countup_running) {
    g_countup_start_time = millis() - g_countup_elapsed_time;
    g_countup_running = true;
  }
  apserver.send(200, "text/plain", "Timer started");
}

void handleStopTimer() {
  if (g_countup_running) {
    g_countup_elapsed_time = millis() - g_countup_start_time;
    g_countup_running = false;
  }
  apserver.send(200, "text/plain", "Timer stopped");
}

void handleResetTimer() {
  g_countup_start_time = 0;
  g_countup_elapsed_time = 0;
  g_countup_running = false;
  apserver.send(200, "text/plain", "Timer reset");
}

void handleGetTimer() {
  unsigned long current_time = 0;
  if (g_countup_running) {
    current_time = millis() - g_countup_start_time;
  } else {
    current_time = g_countup_elapsed_time;
  }
  String json = "{\"time\": " + String(current_time) + ", \"running\": " + String(g_countup_running) + "}";
  apserver.send(200, "application/json", json);
}

void handleSelectNetwork( void )
{
  String lssid;
  int32_t rssi;
  uint8_t encryptionType;
  uint8_t* bssid;
  int32_t channel;
  bool hidden;  
  String response = FPSTR(respTemplate);
  String htmlScanResult = "<div class='center'><a href='select-network' ><input class='button active-btn' type='submit' value='REFRESH'/></a><div style='color: gray;'>Select your network...</div><table class='table-container'><tr><th style='width:85%'></th><th></th></tr>";
  
  int scanResult = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  g_networkNum = scanResult;

  if (scanResult == 0)
  {
    Serial.println(F("No networks found"));
  } 
  else if (scanResult > 0)
  {
    Serial.printf(PSTR("%d networks found:\r\n"), scanResult);

    // Print unsorted scan results
    for (int8_t i = 0; i < scanResult; i++)
    {
      WiFi.getNetworkInfo(i, lssid, encryptionType, rssi, bssid, channel, hidden);

      htmlScanResult += "<tr onclick='getId(this)'>";
      htmlScanResult += "<td class='table-underline'>" + lssid + "</td>";
      htmlScanResult += "<td><img class='wifi-level' src='" + String(GET_WIFI_LEVEL_IMG(rssi)) + "'/></td></tr>";

      Serial.printf(PSTR("  %02d: [CH %02d] [%02X:%02X:%02X:%02X:%02X:%02X] %ddBm %c %c %s\r\n"),
                    i,
                    channel,
                    bssid[0], bssid[1], bssid[2],
                    bssid[3], bssid[4], bssid[5],
                    rssi,
                    (encryptionType == ENC_TYPE_NONE) ? ' ' : '*',
                    hidden ? 'H' : 'V',
                    lssid.c_str());
    }
  } else {
    Serial.printf(PSTR("WiFi scan error %d"), scanResult);
  }

  htmlScanResult += "</table></div>";

  response.replace("{{style}}", FPSTR(respStyle));
  response.replace("{{script}}", FPSTR(jsSelectNetwork));
  response.replace("{{body}}", FPSTR(webSelectNetwork));
  response.replace("{{networks}}", htmlScanResult);

  apserver.sendContent( response );
}

void handleWifiConnected( void )
{
  String response = FPSTR(respTemplate);
  
  response.replace("{{style}}", FPSTR(respStyle));
  response.replace("{{body}}", "");
  response.replace("{{script}}", "");

  apserver.sendContent( response );
}

void handleConnectWifi( void )
{
  String lwifipass, errmsg, response = FPSTR(respTemplate);
  uint8_t i = 0;

  if (apserver.hasArg("password"))
  {
    lwifipass = apserver.arg("password");
    
    if (lwifipass.length() >= 8 || lwifipass.length() == 0)
    {
      String lssid;
      int32_t rssi;
      uint8_t encryptionType;
      uint8_t* bssid;
      int32_t channel;
      bool hidden;

      WiFi.getNetworkInfo(g_wifiSsidID, lssid, encryptionType, rssi, bssid, channel, hidden);
      Serial.print("Wifi index: "); Serial.println(g_wifiSsidID, DEC);
      Serial.print("Wifi SSID:  "); Serial.println(lssid);

      for (i = 0; i < lssid.length(); i++)
      {
        EEPROM.write(EP_WIFI_SSID_ADDR + i, lssid[i]);
      }
      EEPROM.write(EP_WIFI_SSID_ADDR + i, 0);
      
      for (i = 0; i < lwifipass.length(); i++)
      {
        EEPROM.write(EP_WIFI_PASSWORD_ADDR + i, lwifipass[i]);
      }
      EEPROM.write(EP_WIFI_PASSWORD_ADDR + i, 0);

      EEPROM.write(EP_START_MODE_ADDR, START_MODE_COUNTER_MAX);
      EEPROM.commit();

      response.replace("{{style}}", FPSTR(respStyle));
      response.replace("{{body}}", FPSTR(webWifiConnected));
      response.replace("{{script}}", "");
      apserver.sendContent( response );

#if 1
      unsigned long lreset_time = millis();
      while (millis() - lreset_time < 3000) {
        yield();
      }
      ESP.restart();
#endif
    } else {
      Serial.print("Invalid Wifi information: ");
      Serial.print(ssid); Serial.print(" - ");
      Serial.print(password); Serial.println(".");
      apserver.send(404, "text/plaian", "Invalid SSID or password");
    }
  }
  else {
    handleNotFound();
  }
}
