#include <Wire.h>
#include <Adafruit_PN532.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <Adafruit_SSD1306.h>

// ============================
// OLED Settings
// ============================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ============================
// Pin Setup
// ============================
#define BUTTON_PIN 4
#define COIN_PIN 2
#define ENTRY_SERVO_PIN 9
#define EXIT_SERVO_PIN 10
#define DISPENSER_SERVO_PIN 3
#define PN532_IRQ   (0xFF)
#define PN532_RESET (0xFF)

// Ultrasonic pins
#define TRIG1 5
#define ECHO1 6
#define TRIG2 7
#define ECHO2 8
#define TRIG3 11
#define ECHO3 12
#define TRIG4 A0
#define ECHO4 A1

// ============================
// Module Objects
// ============================
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo entryServo;
Servo exitServo;
Servo dispenserServo;

// ============================
// RFID Cards
// ============================
uint8_t validCards[][4] = {
  {0x35, 0xE1, 0x7A, 0xD3},
  {0x65, 0x07, 0xD5, 0xD3},
  {0x55, 0xC9, 0x5C, 0xD3},
  {0x35, 0x33, 0x3F, 0xD3}
};
int numCards = 4;

// ============================
// Variables
// ============================
bool counting[4] = {false, false, false, false};
unsigned long startTime[4] = {0, 0, 0, 0};
int paymentDue[4] = {0, 0, 0, 0};
int coinsInserted[4] = {0, 0, 0, 0};
int currentPayingIndex = -1;
char slotStatus[4] = {'O', 'O', 'O', 'O'}; // O=open, X=occupied, R=reserved
bool oledAvailable = true;
bool awaitingPayment[4] = {false, false, false, false};
bool reservedStatus[4] = {false, false, false, false}; // reservation overlay from host

// ============================
// Card dispense queue (0..3)
// ============================
int availableQueue[4] = {0, 1, 2, 3};
int queueHead = 0;  // index of next item to pop
int queueSize = 4;  // number of items present

int popNextCardFromQueue() {
  if (queueSize == 0) return -1;
  int idx = availableQueue[queueHead];
  queueHead = (queueHead + 1) % 4;
  queueSize--;
  return idx;
}

void pushReturnedCardToQueue(int idx) {
  if (queueSize >= 4) return; // should not happen
  int insertPos = (queueHead + queueSize) % 4;
  availableQueue[insertPos] = idx;
  queueSize++;
}

void removeCardFromQueueIfPresent(int idx) {
  if (queueSize == 0) return;
  int newQueue[4];
  int newSize = 0;
  for (int k = 0; k < queueSize; k++) {
    int pos = (queueHead + k) % 4;
    int val = availableQueue[pos];
    if (val != idx) {
      newQueue[newSize++] = val;
    }
  }
  // write back
  for (int k = 0; k < newSize; k++) {
    availableQueue[k] = newQueue[k];
  }
  queueHead = 0;
  queueSize = newSize;
}

// ============================
// I2C Scanner (debug aid)
// ============================
void scanI2C() {
  Serial.println("Scanning I2C bus...");
  byte count = 0;
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      count++;
    }
  }
  if (count == 0) {
    Serial.println("No I2C devices found");
  }
}

// ============================
// Interrupt Function for Coin
// ============================
void coinInserted() {
  if (currentPayingIndex != -1) {
    coinsInserted[currentPayingIndex]++;
  }
}

// ============================
// Ultrasonic Distance Function
// ============================
long getDistance(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long duration = pulseIn(echo, HIGH, 20000);
  long distance = duration * 0.034 / 2;
  return distance;
}

// ============================
// Serial Commands (host -> MCU)
// ============================
void readHostCommands() {
  // Expected format: RESV:O,R,O,O\n
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("RESV:")) {
      String payload = line.substring(5);
      int idx = 0;
      for (int i = 0; i < 4; i++) {
        int comma = payload.indexOf(',');
        String token;
        if (comma == -1) {
          token = payload;
        } else {
          token = payload.substring(0, comma);
        }
        token.trim();
        reservedStatus[i] = (token == "R");
        if (comma == -1) break;
        payload.remove(0, comma + 1);
      }
    } else if (line.startsWith("START:")) {
      // START:1..4 -> begin counting for that spot/card (1-based index)
      String idxStr = line.substring(6);
      idxStr.trim();
      int oneBased = idxStr.toInt();
      if (oneBased >= 1 && oneBased <= 4) {
        int i = oneBased - 1;
        if (!counting[i] && !awaitingPayment[i]) {
          counting[i] = true;
          startTime[i] = millis();
          // Ensure the assigned card is removed from the dispense queue
          removeCardFromQueueIfPresent(i);
          Serial.print("Started timer for Card ");
          Serial.println(i + 1);
        }
      }
    }
  }
}

// ============================
// OLED Update Function
// ============================
void updateOLED() {
  if (!oledAvailable) return;

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("S1:"); display.print(slotStatus[0]);
  display.setCursor(64, 0);
  display.print("S2:"); display.print(slotStatus[1]);

  display.setCursor(0, 16);
  display.print("S3:"); display.print(slotStatus[2]);
  display.setCursor(64, 16);
  display.print("S4:"); display.print(slotStatus[3]);

  display.display();
}

// ============================
// Setup
// ============================
void setup() {
  Serial.begin(9600);

  // Button & Coin
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinInserted, FALLING);

  // Ultrasonic pins
  pinMode(TRIG1, OUTPUT); pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT); pinMode(ECHO2, INPUT);
  pinMode(TRIG3, OUTPUT); pinMode(ECHO3, INPUT);
  pinMode(TRIG4, OUTPUT); pinMode(ECHO4, INPUT);

  // Servos
  entryServo.attach(ENTRY_SERVO_PIN);
  exitServo.attach(EXIT_SERVO_PIN);
  dispenserServo.attach(DISPENSER_SERVO_PIN);
  entryServo.write(0);
  exitServo.write(0);
  dispenserServo.write(0);

  // Initialize I2C before using any I2C devices (LCD, OLED, PN532)
  Wire.begin();
  // Faster I2C for SSD1306 stability
  #if defined(TWBR)
    Wire.setClock(400000);
  #endif

  // Optional: scan to verify addresses (expects 0x27 for LCD and 0x3C/0x3D for OLED)
  scanI2C();

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();

  // OLED (try common I2C addresses 0x3C then 0x3D)
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED 0x3C not found, trying 0x3D...");
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("OLED not found at 0x3C or 0x3D!");
      oledAvailable = false;
    } else {
      oledAvailable = true;
      display.clearDisplay();
      display.display();
    }
  } else {
    oledAvailable = true;
    display.clearDisplay();
    display.display();
  }

  // PN532
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    lcd.clear();
    lcd.print("PN532 not found!");
    Serial.println("PN532 not detected. Check wiring!");
    while (1);
  }
  nfc.SAMConfig();

  Serial.println("System Ready.");
}

// ============================
// Match RFID UID
// ============================
int matchCard(uint8_t *uid) {
  for (int i = 0; i < numCards; i++) {
    bool match = true;
    for (int j = 0; j < 4; j++) {
      if (uid[j] != validCards[i][j]) {
        match = false;
        break;
      }
    }
    if (match) return i;
  }
  return -1;
}

// ============================
// Main Loop
// ============================
void loop() {
  // === Update slot status from ultrasonic ===
  long d1 = getDistance(TRIG1, ECHO1);
  long d2 = getDistance(TRIG2, ECHO2);
  long d3 = getDistance(TRIG3, ECHO3);
  long d4 = getDistance(TRIG4, ECHO4);

  // Apply occupancy based on ultrasonic only; show 'R' while a session
  // is active (counting or awaitingPayment) or host reservation overlay.
  bool occ0 = (d1 < 15);
  bool occ1 = (d2 < 15);
  bool occ2 = (d3 < 15);
  bool occ3 = (d4 < 15);

  slotStatus[0] = occ0 ? 'X' : ((counting[0] || awaitingPayment[0] || reservedStatus[0]) ? 'R' : 'O');
  slotStatus[1] = occ1 ? 'X' : ((counting[1] || awaitingPayment[1] || reservedStatus[1]) ? 'R' : 'O');
  slotStatus[2] = occ2 ? 'X' : ((counting[2] || awaitingPayment[2] || reservedStatus[2]) ? 'R' : 'O');
  slotStatus[3] = occ3 ? 'X' : ((counting[3] || awaitingPayment[3] || reservedStatus[3]) ? 'R' : 'O');

  updateOLED();

  // Report slot occupancy to host as: SLOTS:O,X,O,O\n
  Serial.print("SLOTS:");
  Serial.print(slotStatus[0]);
  Serial.print(",");
  Serial.print(slotStatus[1]);
  Serial.print(",");
  Serial.print(slotStatus[2]);
  Serial.print(",");
  Serial.println(slotStatus[3]);

  // Process any incoming reservation updates from host
  readHostCommands();

  // === ENTRY BUTTON PRESSED ===
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(200);
    Serial.println("Button pressed - dispensing next card and opening gate...");

    int nextCard = popNextCardFromQueue();
    if (nextCard != -1) {
      // Begin session for this card index
      counting[nextCard] = true;
      startTime[nextCard] = millis();

      // Log assigned card (1-based for display)
      Serial.print("Dispensed Card ");
      Serial.println(nextCard + 1);

      // Physically dispense the card
      dispenserServo.write(90);
      delay(700);
      dispenserServo.write(0);

      // Open entry gate
      entryServo.write(90);
      delay(5000);
      entryServo.write(0);
      Serial.println("Entry gate closed.");

      // LCD: suppress non-payment messages
      delay(1000);
      return;
    }

    Serial.println("No available card/slot.");
    delay(2000);
  }

  // === RFID SCAN ===
  uint8_t success;
  uint8_t uid[7];
  uint8_t uidLength;
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100);

  if (success) {
    int cardIndex = matchCard(uid);

    if (cardIndex != -1 && counting[cardIndex]) {
      unsigned long totalTime = (millis() - startTime[cardIndex]) / 1000;
      counting[cardIndex] = false;

      paymentDue[cardIndex] = ((totalTime + 4) / 5) * 5; // ₱5 per 5 sec
      coinsInserted[cardIndex] = 0;
      awaitingPayment[cardIndex] = true;

      Serial.print("Card ");
      Serial.print(cardIndex + 1);
      Serial.print(" stayed ");
      Serial.print(totalTime);
      Serial.print("s -> Pay ");
      Serial.print(paymentDue[cardIndex]);
      Serial.println(" PHP");

      // If no one is currently paying, show this card on LCD
      if (currentPayingIndex == -1) {
        currentPayingIndex = cardIndex;
        lcd.clear();
        lcd.print("Card ");
        lcd.print(cardIndex + 1);
        lcd.setCursor(0, 1);
        lcd.print("Pay: ");
        lcd.print(paymentDue[cardIndex]);
        lcd.print(" PHP");
      } else {
        Serial.print("Queued for payment: Card ");
        Serial.println(cardIndex + 1);
      }
    }
  }

  // === PAYMENT PROCESSING (non-blocking) ===
  if (currentPayingIndex != -1) {
    int idx = currentPayingIndex;
    int coinsNeeded = paymentDue[idx] / 5;
    int remaining = (coinsNeeded - coinsInserted[idx]) * 5;
    if (remaining < 0) remaining = 0;

    // Update LCD with remaining amount
    lcd.setCursor(0, 0);
    lcd.print("Card ");
    lcd.print(idx + 1);
    lcd.print("       ");
    lcd.setCursor(0, 1);
    lcd.print("Insert: ");
    lcd.print(remaining);
    lcd.print(" PHP   ");

    if (coinsInserted[idx] >= coinsNeeded) {
      // Show thank you message briefly, then clear
      lcd.clear();
      lcd.print("Thank You!");
      delay(1500);
      lcd.clear();

      Serial.print("Card ");
      Serial.print(idx + 1);
      Serial.println(" payment complete.");
      delay(1000);

      // Exit gate handling
      exitServo.write(90);
      delay(5000);
      exitServo.write(0);
      Serial.println("Exit gate closed.");

      // Clear payment state
      awaitingPayment[idx] = false;
      paymentDue[idx] = 0;
      coinsInserted[idx] = 0;
      currentPayingIndex = -1;

      // Returned card goes to the end of the queue
      pushReturnedCardToQueue(idx);

      // Clear reservation overlay for this spot and inform host
      reservedStatus[idx] = false;
      Serial.print("PAID:");
      Serial.println(idx + 1);

      // Switch to next queued card if any
      for (int i = 0; i < numCards; i++) {
        if (awaitingPayment[i]) {
          currentPayingIndex = i;
          lcd.clear();
          lcd.print("Card ");
          lcd.print(i + 1);
          lcd.setCursor(0, 1);
          lcd.print("Pay: ");
          lcd.print(paymentDue[i]);
          lcd.print(" PHP");
          break;
        }
      }
    }
  }

  delay(500); // update interval
}
