#include <Wire.h>
#include <Adafruit_PN532.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <Tiny4kOLED.h>

// ============================
// OLED Settings - Using Tiny4kOLED
// ============================

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
int capacityHold = 0; // number of cards to hold back for reservations

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
      Serial.print("Received RESV: ");
      Serial.println(payload);
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
    } else if (line.startsWith("HERE:")) {
      // HERE:1..4 -> reserved user arrived, assign next card in queue (same as walk-ins)
      String idxStr = line.substring(5);
      idxStr.trim();
      int oneBased = idxStr.toInt();
      Serial.print("HERE command for slot ");
      Serial.println(oneBased);
      if (oneBased >= 1 && oneBased <= 4) {
        // For reserved users, use same sequential queue logic as walk-ins
        int allowedAvailable = queueSize - capacityHold;
        if (allowedAvailable <= 0) {
          Serial.println("No available card/slot (capacity held) for reserved user.");
        } else {
          // Skip indices that are reserved/held by host (reservedStatus[i])
          int nextCard = -1;
          for (int attempt = 0; attempt < 4; attempt++) {
            int candidate = popNextCardFromQueue();
            if (candidate == -1) break;
            if (reservedStatus[candidate]) {
              // Put back to end of queue as it's held
              pushReturnedCardToQueue(candidate);
              continue;
            }
            nextCard = candidate;
            break;
          }

          if (nextCard != -1) {
            counting[nextCard] = true;
            startTime[nextCard] = millis();

            Serial.print("Assigned next card ");
            Serial.print(nextCard + 1);
            Serial.println(" to reserved user");
            
            // Report card dispensed to server for queue sync
            Serial.print("DISP:");
            Serial.println(nextCard + 1);

            Serial.println("Dispensing card and opening gate...");
            // Dispense card for reserved user
            dispenserServo.write(90);
            delay(700);
            dispenserServo.write(0);

            // Open entry gate
            entryServo.write(90);
            delay(5000);
            entryServo.write(0);
            Serial.println("Gate closed.");
          } else {
            Serial.println("No available card/slot for reserved user");
          }
        }
      } else {
        Serial.println("Invalid slot number");
      }
    } else if (line.startsWith("CAP:")) {
      // CAP:n -> hold back last n cards for reservations
      String num = line.substring(4);
      num.trim();
      int n = num.toInt();
      if (n < 0) n = 0;
      if (n > 4) n = 4;
      capacityHold = n;
      Serial.print("Capacity hold set to ");
      Serial.println(capacityHold);
    }
  }
}

// ============================
// OLED Update Function - Display Payment Info ONLY
// ============================
void updateOLED() {
  if (!oledAvailable) return;

  oled.clear();
  oled.setFont(FONT8X16);

  if (currentPayingIndex != -1) {
    // Show payment info when someone is paying
    int idx = currentPayingIndex;
    int coinsNeeded = paymentDue[idx] / 5;
    int remaining = (coinsNeeded - coinsInserted[idx]) * 5;
    if (remaining < 0) remaining = 0;

    // Each page = 8 pixels, so use y = 0, 2, 4, etc.
    oled.setCursor(0, 0);   // top line
    oled.print("Slot ");
    oled.print(idx + 1);
    oled.print(" Payment");

    oled.setCursor(0, 2);   // second line
    oled.print("Php ");
    oled.print(remaining);
  } else {
    // Show welcome message when no payment
    oled.setCursor(0, 0);
    oled.print("Welcome to");
    oled.setCursor(0, 2);
    oled.print("Parking System");
  }
}


// ============================
// LCD Update Function - Display Slot Status
// ============================
void updateLCD() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("S1:"); lcd.print(slotStatus[0]);
  lcd.print(" S2:"); lcd.print(slotStatus[1]);
  lcd.setCursor(0, 1);
  lcd.print("S3:"); lcd.print(slotStatus[2]);
  lcd.print(" S4:"); lcd.print(slotStatus[3]);
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

  // OLED using Tiny4kOLED
  oled.begin();
  oledAvailable = true;
  oled.clear();
  oled.on();
  Serial.println("OLED initialized with Tiny4kOLED!");

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
  updateLCD();

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

    int allowedAvailable = queueSize - capacityHold;
    if (allowedAvailable <= 0) {
      Serial.println("No available card/slot (capacity held).");
      delay(2000);
    } else {
      // Skip indices that are reserved/held by host (reservedStatus[i])
      int nextCard = -1;
      for (int attempt = 0; attempt < 4; attempt++) {
        int candidate = popNextCardFromQueue();
        if (candidate == -1) break;
        if (reservedStatus[candidate]) {
          // Put back to end of queue as it's held
          pushReturnedCardToQueue(candidate);
          continue;
        }
        nextCard = candidate;
        break;
      }
      if (nextCard != -1) {
      // Begin session for this card index
      counting[nextCard] = true;
      startTime[nextCard] = millis();

      // Log assigned card (1-based for display)
      Serial.print("Dispensed Card ");
      Serial.println(nextCard + 1);
      
      // Report card dispensed to server for queue sync
      Serial.print("DISP:");
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

      // If no one is currently paying, show this card
      if (currentPayingIndex == -1) {
        currentPayingIndex = cardIndex;
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

    // Payment info now handled by updateLCD() function

    if (coinsInserted[idx] >= coinsNeeded) {
      
     // Show thank you on OLED
      oled.clear();
      oled.setFont(FONT8X16);
      oled.setCursor(0, 0);
      oled.print("Thank You!");
      oled.setCursor(0, 2);
      oled.print("Payment OK");
      
      delay(2000); // Show thank you for 2 seconds
      
      oled.clear();

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
          break;
        }
      }
    }
  }

  delay(500); // update interval
}
