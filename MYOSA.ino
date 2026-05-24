/*
 * PROJECT: KAIROS - Gesture Controlled Smart Remote
 * AUTHOR: Swaraj
 * DESCRIPTION: 
 * A multi-functional smart wearable powered by ESP32, MPU6050, and OLED.
 * Features include:
 * - Air Mouse & Media Control
 * - Instagram Reel Scroller
 * - Laser Pointer & Presentation Clicker
 * - Bluetooth Camera Shutter
 * - Environment Monitoring (BMP180)
 * - "Sentry Mode" (Breath Detection Wake-up)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP085.h> 
#include <BleCombo.h> 
#include "LightProximityAndGesture.h" // Ensure this library is in your sketch folder
#include <Preferences.h> 
#include <esp_mac.h>     

// ==========================================
//           HARDWARE CONFIGURATION
// ==========================================
#define SDA_PIN               21
#define SCL_PIN               22
#define SCREEN_WIDTH          128
#define SCREEN_HEIGHT         64
#define OLED_ADDR             0x3C
#define MPU_ADDR_1            0x69
#define MPU_ADDR_2            0x68

// ==========================================
//               SETTINGS
// ==========================================
#define SHAKE_THRESHOLD       14.0     
#define TILT_MENU_THRESHOLD   4.5  
#define TILT_SCROLL_DEADZONE  3.0 
#define PROX_TRIGGER          50          
#define MENU_SCROLL_DELAY     350    
#define MAX_DEVICES           3            
#define SLEEP_TIMEOUT         30000      // 30 Seconds to Sentry Mode
#define BREATH_THRESHOLD      30         // Sensitivity of Breath Detection

// Mouse & Gyro Settings
#define MOUSE_SENSITIVITY     25     
#define GYRO_DEADZONE         0.08       

// ==========================================
//           GLOBALS & OBJECTS
// ==========================================

// Menu Structure
const char* menuItems[] = { "MOUSE", "MEDIA", "REELS", "SLIDES", "CAMERA", "READER", "ENV", "DEVICE" };
const int menuLength = 8;

// System States
enum SystemState { 
  DASHBOARD, MOUSE_MODE, MEDIA_MODE, REEL_MODE, 
  PRESENTATION_MODE, CAMERA_MODE, READER_MODE, 
  ENV_MODE, DEVICE_SELECT_MODE, SLEEP_MODE 
};

SystemState currentState = DASHBOARD;
int menuOption = 0; 

// Hardware Objects
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_MPU6050 mpu;
LightProximityAndGesture apds;
Adafruit_BMP085 bmp; 
Preferences prefs;

// Timers & Flags
unsigned long globalTimer = 0;
unsigned long proxTimer = 0; 
unsigned long actionTimer = 0; 
unsigned long menuTimer = 0;   
unsigned long lastInputTime = 0;

bool isClicking = false;
bool shakeDebounce = false;
bool isHolding = false; 
bool laserActive = false; 

// Multi-Device Variables
int currentDeviceID = 1; 

// Environmental Variables
float baselinePressure = 0;

// Calibration Offsets
float gyroXoffset = 0, gyroYoffset = 0, gyroZoffset = 0;

// Camera Timer Variables
bool isCamTimerRunning = false;
int camCount = 5;
unsigned long camTick = 0;

// ==========================================
//           HELPER FUNCTIONS
// ==========================================

// Modify MAC Address for Multi-Device Pairing
void setDeviceIdentity(int id) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA); 
  mac[5] = mac[5] + id; 
  esp_base_mac_addr_set(mac);
}

// Calibrate Gyro on Startup
void calibrateMPU() {
  display.clearDisplay();
  display.setCursor(20, 20); display.setTextSize(1); display.println(F("CALIBRATING..."));
  display.setCursor(25, 40); display.println(F("DONT MOVE!"));
  display.display();

  float xSum = 0, ySum = 0, zSum = 0;
  int numReadings = 100;

  for (int i = 0; i < numReadings; i++) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    xSum += g.gyro.x;
    ySum += g.gyro.y;
    zSum += g.gyro.z;
    delay(10);
  }
  gyroXoffset = xSum / numReadings;
  gyroYoffset = ySum / numReadings;
  gyroZoffset = zSum / numReadings;
}

// Draw UI Feedback Popups
void drawFeedback(String title, String subtitle) {
  display.clearDisplay();
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE); 
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(5, 4); display.setTextSize(1); display.println(title);
  
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 30); display.setTextSize(2); display.println(subtitle);
  display.display();
}

// Main Menu UI
void drawMenu() {
  display.clearDisplay();
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);
  display.setCursor(0, 0); display.setTextSize(1); 
  display.print(F("MENU | DASH:")); display.print(currentDeviceID); 
  
  if (menuOption > 0) {
    display.setCursor(10, 20); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.println(menuItems[menuOption - 1]);
  }

  display.fillRect(0, 32, 128, 20, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(5, 35); display.setTextSize(2); 
  display.print(F("> ")); display.println(menuItems[menuOption]);

  if (menuOption < menuLength - 1) {
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 55); display.setTextSize(1); 
    display.println(menuItems[menuOption + 1]);
  }
  display.display();
}

void resetSleepTimer() {
  lastInputTime = millis();
}

void enterSleep() {
  currentState = SLEEP_MODE;
  display.clearDisplay();
  display.display(); // Screen Off
}

// Wake Up Routine: Wakes remote and sends wake signal to PC/Phone
void wakeUpRoutine() {
  currentState = DASHBOARD;
  display.clearDisplay();
  display.setCursor(10, 20); display.setTextSize(2); display.println(F("WAKING"));
  display.setCursor(35, 45); display.setTextSize(2); display.print(F("DEV ")); display.println(currentDeviceID);
  display.display();
  
  // Universal Wake Signal (Shake mouse + Ctrl Key)
  Mouse.move(15, 15);
  delay(50);
  Mouse.move(-15, -15);
  delay(50);
  Keyboard.press(KEY_LEFT_CTRL); 
  delay(50);
  Keyboard.releaseAll();
  
  delay(800); 
  resetSleepTimer();
}

// Switch Bluetooth Identity
void switchDevice(int newID) {
  display.clearDisplay();
  display.setCursor(10, 20); display.setTextSize(1); display.println(F("SWITCHING TO"));
  display.setCursor(30, 40); display.setTextSize(2); display.print(F("DASH ")); display.println(newID);
  display.display();
  
  prefs.putInt("devID", newID);
  prefs.end();
  
  delay(500);
  ESP.restart(); 
}

// ==========================================
//               SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // 1. Load Profile
  prefs.begin("rem_settings", false);
  currentDeviceID = prefs.getInt("devID", 1); 
  if(currentDeviceID < 1 || currentDeviceID > MAX_DEVICES) currentDeviceID = 1;

  // 2. Set Identity
  setDeviceIdentity(currentDeviceID);

  // 3. Init Hardware
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000); 

  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED Error")); for(;;);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  if (!mpu.begin(MPU_ADDR_1)) {
    if (!mpu.begin(MPU_ADDR_2)) Serial.println(F("MPU Error"));
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  calibrateMPU();

  // Show Active Dashboard
  display.clearDisplay();
  display.setCursor(15, 10); display.setTextSize(2); display.print(F("DASHBOARD")); 
  display.setCursor(60, 35); display.setTextSize(3); display.println(currentDeviceID);
  display.display();

  if (!apds.begin()) Serial.println(F("APDS Error"));
  apds.enableProximitySensor(DISABLE);
  apds.enableGestureSensor(DISABLE);

  if (!bmp.begin()) { 
    Serial.println("BMP180 Error"); 
  } else {
    baselinePressure = bmp.readPressure(); // Init Pressure Baseline
  }

  // 4. Init Bluetooth
  Keyboard.begin();
  Mouse.begin();
  
  lastInputTime = millis();
  delay(1500);
}

// ==========================================
//               MAIN LOOP
// ==========================================
void loop() {
  
  // --- CHECK SLEEP ---
  if (currentState != SLEEP_MODE && (millis() - lastInputTime > SLEEP_TIMEOUT)) {
    enterSleep();
  }

  // --- SENTRY MODE (BREATH DETECTION) ---
  if (currentState == SLEEP_MODE) {
    float currentP = bmp.readPressure();
    
    // Check for rapid pressure spike (Breath)
    if (currentP - baselinePressure > BREATH_THRESHOLD) {
      wakeUpRoutine(); 
    }
    
    // Update baseline slowly to ignore weather changes
    baselinePressure = (baselinePressure * 0.95) + (currentP * 0.05);
    
    delay(100); 
    return; // Halt other logic while sleeping
  }

  // --- NORMAL OPERATION ---

  if (!Keyboard.isConnected()) {
    if (millis() - globalTimer > 1000) {
      display.clearDisplay();
      display.setCursor(20, 10); display.setTextSize(2); display.println(F("PAIRING"));
      display.setCursor(10, 35); display.setTextSize(1); display.print(F("Profile: Dash ")); display.println(currentDeviceID);
      display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
      display.display();
      globalTimer = millis();
    }
    return; 
  }

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // Movement resets sleep timer
  if (abs(g.gyro.x) > 2 || abs(g.gyro.y) > 2 || abs(g.gyro.z) > 2) {
    resetSleepTimer();
  }

  // --- EXIT LOGIC (SHAKE TO HOME) ---
  if ((abs(a.acceleration.x) > SHAKE_THRESHOLD) || (abs(a.acceleration.y) > SHAKE_THRESHOLD)) {
    if (!shakeDebounce && currentState != DASHBOARD) {
      // Release all inputs
      Mouse.release(MOUSE_LEFT);
      Keyboard.releaseAll();
      isClicking = false;
      laserActive = false;
      isCamTimerRunning = false;
      
      // Special exit for Camera Mode
      if (currentState == CAMERA_MODE) {
        drawFeedback("SYSTEM", "CLOSING APP");
        Keyboard.press(KEY_LEFT_ALT); Keyboard.press(KEY_F4); delay(100); Keyboard.releaseAll();
        delay(800);
      }
      
      currentState = DASHBOARD;
      drawFeedback("SYSTEM", "HOME");
      delay(800);
      
      shakeDebounce = true;
      isHolding = false;
      proxTimer = 0;
      resetSleepTimer();
      return; 
    }
  } else {
    shakeDebounce = false;
  }

  // --- STATE MACHINE ---
  switch(currentState) {
    
    // ------------------------------------------
    // 1. DASHBOARD & MENU
    // ------------------------------------------
    case DASHBOARD:
      if (millis() - menuTimer > MENU_SCROLL_DELAY) {
        // Tilt X-axis to scroll menu
        if (a.acceleration.x > TILT_MENU_THRESHOLD) {
           if(menuOption < menuLength - 1) { menuOption++; menuTimer = millis(); resetSleepTimer(); }
        }
        else if (a.acceleration.x < -TILT_MENU_THRESHOLD) {
           if(menuOption > 0) { menuOption--; menuTimer = millis(); resetSleepTimer(); }
        }
      }
      drawMenu();

      uint8_t prox; apds.readProximity(&prox);
      if (prox > PROX_TRIGGER) {
        if (!isHolding) { isHolding = true; proxTimer = millis(); }
        
        // Long Hold (2s): Quick Device Switch
        if (millis() - proxTimer > 2000) {
           int nextID = currentDeviceID + 1;
           if (nextID > MAX_DEVICES) nextID = 1;
           switchDevice(nextID);
        }
      } else {
        if (isHolding) {
           isHolding = false;
           // Short Click (<2s): Enter Mode
           if (millis() - proxTimer < 2000) {
              drawFeedback("LOADING", menuItems[menuOption]);
              delay(500);

              if (menuOption == 0) currentState = MOUSE_MODE;
              else if (menuOption == 1) currentState = MEDIA_MODE;
              else if (menuOption == 2) currentState = REEL_MODE;
              else if (menuOption == 3) currentState = PRESENTATION_MODE;
              else if (menuOption == 4) {
                   currentState = CAMERA_MODE;
                   drawFeedback("CAMERA", "LAUNCHING");
                   // Launch Camera App (Windows Shortcut)
                   Keyboard.press(KEY_LEFT_GUI); delay(100); Keyboard.releaseAll(); delay(200);
                   Keyboard.print("Camera"); delay(500);
                   Keyboard.press(KEY_RETURN); delay(100); Keyboard.releaseAll();
                   delay(2000);
              }
              else if (menuOption == 5) currentState = READER_MODE;
              else if (menuOption == 6) currentState = ENV_MODE;
              else if (menuOption == 7) currentState = DEVICE_SELECT_MODE;
              
              resetSleepTimer();
           }
        }
      }
      break;

    // ------------------------------------------
    // 2. DEVICE SELECT MODE
    // ------------------------------------------
    case DEVICE_SELECT_MODE: {
      if (millis() - menuTimer > MENU_SCROLL_DELAY) {
        if (a.acceleration.x > TILT_MENU_THRESHOLD) {
           if(currentDeviceID < MAX_DEVICES) { currentDeviceID++; menuTimer = millis(); resetSleepTimer(); }
        }
        else if (a.acceleration.x < -TILT_MENU_THRESHOLD) {
           if(currentDeviceID > 1) { currentDeviceID--; menuTimer = millis(); resetSleepTimer(); }
        }
      }

      display.clearDisplay();
      display.setCursor(10, 0); display.setTextSize(2); display.println(F("SWITCH TO:"));
      
      // Draw 3 Boxes
      display.drawRect(5, 30, 30, 30, SSD1306_WHITE);
      display.drawRect(45, 30, 30, 30, SSD1306_WHITE);
      display.drawRect(85, 30, 30, 30, SSD1306_WHITE);
      
      // Highlight Active Box
      int xPos = 5 + (currentDeviceID - 1) * 40;
      display.fillRect(xPos, 30, 30, 30, SSD1306_WHITE);

      display.setTextSize(2);
      display.setTextColor(currentDeviceID == 1 ? SSD1306_BLACK : SSD1306_WHITE); display.setCursor(12, 37); display.print("1");
      display.setTextColor(currentDeviceID == 2 ? SSD1306_BLACK : SSD1306_WHITE); display.setCursor(52, 37); display.print("2");
      display.setTextColor(currentDeviceID == 3 ? SSD1306_BLACK : SSD1306_WHITE); display.setCursor(92, 37); display.print("3");
      display.display();
      display.setTextColor(SSD1306_WHITE); 

      uint8_t dProx; apds.readProximity(&dProx);
      if (dProx > PROX_TRIGGER) {
        switchDevice(currentDeviceID);
      }
      break;
    }

    // ------------------------------------------
    // 3. MOUSE MODE
    // ------------------------------------------
    case MOUSE_MODE: { 
      // Gyro Logic
      float gx = g.gyro.z - gyroZoffset; 
      float gy = g.gyro.y - gyroYoffset; 
      
      // Deadzone
      if (abs(gx) < GYRO_DEADZONE) gx = 0;
      if (abs(gy) < GYRO_DEADZONE) gy = 0;

      // Constrain movement to prevent overflow/glitching
      int moveX = constrain(-gx * MOUSE_SENSITIVITY, -127, 127);
      int moveY = constrain(-gy * MOUSE_SENSITIVITY, -127, 127);
      
      Mouse.move(moveX, moveY);

      // Click Logic (Proximity)
      uint8_t mProx; apds.readProximity(&mProx);
      if (mProx > PROX_TRIGGER) {
        if (!isClicking) { Mouse.press(MOUSE_LEFT); isClicking = true; resetSleepTimer(); }
        display.clearDisplay(); display.fillCircle(64, 32, 10, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK);
        display.setCursor(45, 28); display.setTextSize(1); display.println(F("CLICK")); display.display();
      } else {
        if (isClicking) { Mouse.release(MOUSE_LEFT); isClicking = false; resetSleepTimer(); }
        
        // Gesture Shortcuts
        if (apds.isGestureAvailable()) {
           resetSleepTimer();
           int gst = apds.readGesture();
           if (gst == DIR_LEFT) { Keyboard.press(KEY_LEFT_CTRL); Keyboard.write('c'); Keyboard.releaseAll(); drawFeedback("ACTION", "COPY"); delay(800); }
           else if (gst == DIR_RIGHT) { Keyboard.press(KEY_LEFT_CTRL); Keyboard.write('v'); Keyboard.releaseAll(); drawFeedback("ACTION", "PASTE"); delay(800); }
           else if (gst == DIR_UP) { Keyboard.press(KEY_LEFT_ALT); Keyboard.press(KEY_TAB); delay(100); Keyboard.releaseAll(); drawFeedback("ACTION", "SWITCH"); delay(800); }
           else if (gst == DIR_DOWN) { Keyboard.press(KEY_LEFT_GUI); Keyboard.write('d'); Keyboard.releaseAll(); drawFeedback("ACTION", "DESKTOP"); delay(800); }
        }
        
        // UI Refresh
        if (millis() - globalTimer > 200) {
           display.clearDisplay(); display.setCursor(0,0); display.setTextSize(2); display.println(F("MOUSE")); 
           display.setTextSize(1); display.setCursor(0,25); display.println(F("Hold: Click"));
           display.setCursor(0,35); display.println(F("L/R : Copy/Paste"));
           display.setCursor(0,45); display.println(F("U/D : Tab/Desk"));
           display.display(); globalTimer = millis();
        }
      }
      break;
    } 

    // ------------------------------------------
    // 4. MEDIA MODE
    // ------------------------------------------
    case MEDIA_MODE: {
      uint8_t medProx; apds.readProximity(&medProx);
      if (medProx > PROX_TRIGGER) {
         if (!isHolding) { isHolding = true; proxTimer = millis(); resetSleepTimer(); }
         unsigned long dur = millis() - proxTimer;
         display.clearDisplay(); display.setCursor(0,0); display.setTextSize(2); display.println(F("HOLDING..."));
         int w = map(constrain(dur, 0, 5000), 0, 5000, 0, 128);
         display.fillRect(0, 40, w, 14, SSD1306_WHITE);
         display.setCursor(0, 20); display.setTextSize(1);
         if (dur < 3000) display.println(F("Keep Holding..."));
         else if (dur < 5000) display.println(F("Release: PAUSE"));
         else display.println(F("Release: MUTE"));
         display.display();
      } else {
         if (isHolding) {
             unsigned long t = millis() - proxTimer; isHolding = false; resetSleepTimer();
             if (t >= 5000) { Keyboard.write(KEY_MEDIA_MUTE); drawFeedback("AUDIO", "MUTE/UNMUTE"); delay(1000); }
             else if (t >= 3000) { Keyboard.write(KEY_MEDIA_PLAY_PAUSE); drawFeedback("MEDIA", "PLAY/PAUSE"); delay(1000); }
         }
         if (!isHolding && apds.isGestureAvailable()) {
            resetSleepTimer();
            int gst = apds.readGesture();
            if (gst == DIR_UP) { Keyboard.write(KEY_MEDIA_VOLUME_UP); drawFeedback("VOLUME", "UP +"); delay(150); }
            else if (gst == DIR_DOWN) { Keyboard.write(KEY_MEDIA_VOLUME_DOWN); drawFeedback("VOLUME", "DOWN -"); delay(150); }
            else if (gst == DIR_RIGHT) { Keyboard.write(KEY_MEDIA_NEXT_TRACK); drawFeedback("TRACK", "NEXT >>"); delay(500); }
            else if (gst == DIR_LEFT) { Keyboard.write(KEY_MEDIA_PREVIOUS_TRACK); drawFeedback("TRACK", "<< PREV"); delay(500); }
         }
         if (!isHolding && millis() - globalTimer > 500) {
             display.clearDisplay(); display.setCursor(0,0); display.setTextSize(2); display.println(F("MEDIA"));
             display.setTextSize(1); display.setCursor(0,30); display.println(F("Swipe: Nav/Vol")); 
             display.setCursor(0,45); display.println(F("Hold: 3s Play/5s Mute"));
             display.display(); globalTimer = millis();
         }
      }
      break;
    }

    // ------------------------------------------
    // 5. REELS MODE
    // ------------------------------------------
    case REEL_MODE: {
      if (apds.isGestureAvailable()) {
         resetSleepTimer();
         int gst = apds.readGesture();
         if (gst == DIR_UP) { Keyboard.write(KEY_DOWN_ARROW); drawFeedback("REELS", "NEXT VIDEO"); delay(300); }
         else if (gst == DIR_DOWN) { Keyboard.write(KEY_UP_ARROW); drawFeedback("REELS", "PREV VIDEO"); delay(300); }
         else if (gst == DIR_RIGHT) { Keyboard.write(KEY_RIGHT_ARROW); drawFeedback("GALLERY", "NEXT >"); delay(300); }
         else if (gst == DIR_LEFT) { Keyboard.write(KEY_LEFT_ARROW); drawFeedback("GALLERY", "< PREV"); delay(300); }
      } 
      else {
         uint8_t rProx; apds.readProximity(&rProx);
         if (rProx > PROX_TRIGGER) {
            if (!isHolding) { isHolding = true; proxTimer = millis(); resetSleepTimer(); }
            if (millis() - proxTimer > 400 && !laserActive) { 
               Keyboard.write(' '); drawFeedback("REELS", "PAUSE/PLAY"); laserActive = true; delay(800); 
            }
         } else {
            isHolding = false; laserActive = false; 
            if (millis() - globalTimer > 500) {
               display.clearDisplay(); display.setCursor(0,0); display.setTextSize(2); display.println(F("REELS/IMG"));
               display.setTextSize(1); display.setCursor(0,30); display.println(F("Swipe U/D: Video")); 
               display.setCursor(0,45); display.println(F("Hold: Pause"));
               display.display(); globalTimer = millis();
            }
         }
      }
      break;
    }

    // ------------------------------------------
    // 6. PRESENTATION (SLIDES) MODE
    // ------------------------------------------
    case PRESENTATION_MODE: {
      // Zoom Logic (Tilt X)
      if (millis() - actionTimer > 250) {
          if (a.acceleration.x > TILT_MENU_THRESHOLD) { 
            Keyboard.press(KEY_LEFT_CTRL); Keyboard.write('+'); Keyboard.releaseAll(); 
            drawFeedback("ZOOM", "IN (+)"); actionTimer = millis(); resetSleepTimer(); 
          }
          else if (a.acceleration.x < -TILT_MENU_THRESHOLD) { 
            Keyboard.press(KEY_LEFT_CTRL); Keyboard.write('-'); Keyboard.releaseAll(); 
            drawFeedback("ZOOM", "OUT (-)"); actionTimer = millis(); resetSleepTimer(); 
          }
      }

      // Proximity (Laser Pointer & Mouse Activation)
      uint8_t pProx; apds.readProximity(&pProx);
      
      if (pProx > PROX_TRIGGER) {
         // Activate Laser & Mouse
         if (!laserActive) { 
            Keyboard.press(KEY_LEFT_CTRL); 
            Mouse.press(MOUSE_LEFT); 
            laserActive = true; 
            resetSleepTimer(); 
         }

         // Mouse Move only when Laser is Active
         float gx = g.gyro.z - gyroZoffset; 
         float gy = g.gyro.y - gyroYoffset; 
         if (abs(gx) < GYRO_DEADZONE) gx = 0;
         if (abs(gy) < GYRO_DEADZONE) gy = 0;
         
         // Lower sensitivity for precision
         int moveX = constrain(-gx * (MOUSE_SENSITIVITY - 5), -127, 127);
         int moveY = constrain(-gy * (MOUSE_SENSITIVITY - 5), -127, 127);
         Mouse.move(moveX, moveY);

         display.clearDisplay(); display.fillCircle(64, 32, 10, SSD1306_WHITE); 
         display.setCursor(35, 50); display.setTextSize(1); display.println(F("LASER")); display.display();
      
      } else {
         // Deactivate Laser
         if (laserActive) { 
            Mouse.release(MOUSE_LEFT); 
            Keyboard.releaseAll(); 
            laserActive = false; 
            resetSleepTimer(); 
         }

         // Slide Navigation Gestures
         if (apds.isGestureAvailable()) {
            resetSleepTimer();
            int gst = apds.readGesture();
            if (gst == DIR_RIGHT) { Keyboard.write(KEY_RIGHT_ARROW); drawFeedback("SLIDE", "NEXT >"); delay(500); }
            else if (gst == DIR_LEFT) { Keyboard.write(KEY_LEFT_ARROW); drawFeedback("SLIDE", "< PREV"); delay(500); }
            else if (gst == DIR_UP) { Keyboard.press(KEY_F5); delay(100); Keyboard.releaseAll(); drawFeedback("CMD", "START (F5)"); delay(1000); }
            else if (gst == DIR_DOWN) { Keyboard.write(KEY_ESC); drawFeedback("CMD", "END (ESC)"); delay(1000); }
         }
         
         if (millis() - globalTimer > 500 && !laserActive) {
            display.clearDisplay(); display.setCursor(0,0); display.setTextSize(2); display.println(F("SLIDES"));
            display.setTextSize(1); display.setCursor(0,30); display.println(F("Swipe: Next/Prev")); 
            display.setCursor(0,45); display.println(F("Tilt : Zoom +/-"));
            display.display(); globalTimer = millis();
         }
      }
      break;
    }

    // ------------------------------------------
    // 7. CAMERA MODE
    // ------------------------------------------
    case CAMERA_MODE: { 
      // Timer Logic
      if (isCamTimerRunning) {
         if (millis() - camTick > 1000) {
            camCount--; camTick = millis();
            display.clearDisplay(); display.setCursor(55, 15); display.setTextSize(4); display.println(camCount); display.display();
            if (camCount <= 0) { Keyboard.write(KEY_RETURN); isCamTimerRunning = false; drawFeedback("CAMERA", "SNAP!"); delay(1000); resetSleepTimer(); }
         }
         return; 
      }
      
      // Camera Gestures
      if (apds.isGestureAvailable()) {
         resetSleepTimer();
         int gst = apds.readGesture();
         if (gst == DIR_UP || gst == DIR_DOWN) { isCamTimerRunning = true; camCount = 6; camTick = millis(); }
         else if (gst == DIR_RIGHT) { Keyboard.write(KEY_DOWN_ARROW); drawFeedback("MODE", "VIDEO"); delay(800); }
         else if (gst == DIR_LEFT) { Keyboard.write(KEY_UP_ARROW); drawFeedback("MODE", "PHOTO"); delay(800); }
      }
      
      if (millis() - globalTimer > 500) {
         display.clearDisplay(); display.setCursor(0,0); display.setTextSize(2); display.println(F("CAMERA"));
         display.setTextSize(1); display.setCursor(0,30); display.println(F("Swipe U/D: Timer")); 
         display.setCursor(0,45); display.println(F("Swipe L/R: Mode")); 
         display.display(); globalTimer = millis();
      }
      break;
    } 

    // ------------------------------------------
    // 8. READER MODE
    // ------------------------------------------
    case READER_MODE: {
      // Tilt to Scroll (Y-axis)
      float tiltY = a.acceleration.y;
      if (abs(tiltY) > TILT_SCROLL_DEADZONE) {
         int delayTime = map(constrain(abs(tiltY), 3.0, 9.0), 3.0, 9.0, 120, 10);
         if (millis() - actionTimer > delayTime) {
             if (tiltY > 0) { Mouse.move(0, 0, -1); display.clearDisplay(); display.setCursor(30, 25); display.setTextSize(2); display.println("DOWN v"); display.display(); }
             else { Mouse.move(0, 0, 1); display.clearDisplay(); display.setCursor(40, 25); display.setTextSize(2); display.println("UP ^"); display.display(); }
             actionTimer = millis();
             resetSleepTimer();
         }
      } else {
         // Swipe to change pages
         if (apds.isGestureAvailable()) {
            resetSleepTimer();
            int gst = apds.readGesture();
            if (gst == DIR_RIGHT) { Keyboard.write(KEY_PAGE_DOWN); drawFeedback("READER", "PAGE DN"); delay(400); }
            if (gst == DIR_LEFT) { Keyboard.write(KEY_PAGE_UP); drawFeedback("READER", "PAGE UP"); delay(400); }
         }
         
         if (millis() - globalTimer > 500) {
             display.clearDisplay(); display.setCursor(0,0); display.setTextSize(2); display.println(F("READER"));
             display.setTextSize(1); display.setCursor(0,30); display.println(F("Tilt : Scroll")); 
             display.setCursor(0,45); display.println(F("Swipe: Page +/-")); 
             display.display(); globalTimer = millis();
         }
      }
      break;
    } 

    // ------------------------------------------
    // 9. ENV MODE
    // ------------------------------------------
    case ENV_MODE: { 
      if (millis() - globalTimer > 1000) {
        float tempC = bmp.readTemperature();
        float pressure = bmp.readPressure();
        float altitude = bmp.readAltitude(101325); 

        display.clearDisplay();
        display.fillRect(0, 0, 128, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
        display.setCursor(25, 3); display.setTextSize(1); display.println(F("ENVIRONMENT"));
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 20); display.setTextSize(1); display.print(F("Temp: "));
        display.setTextSize(2); display.print(tempC, 1); display.setTextSize(1); display.println(F(" C"));
        display.setCursor(0, 40); display.print(F("Pres: "));
        display.print(pressure / 100); display.println(F(" hPa"));
        display.setCursor(0, 52); display.print(F("Alt : "));
        display.print(altitude, 1); display.println(F(" m"));
        display.display();
        
        globalTimer = millis();
        resetSleepTimer();
      }
      break;
    }
  }
}