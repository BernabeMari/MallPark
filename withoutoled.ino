#include <Wire.h>
#include <Adafruit_PN532.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

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
// VIP RFID System
// ============================
String reservedRFIDs[10];
String reservedCustomers[10];
int numReservedRFIDs = 0;
String lastScannedRFID = "";
unsigned long lastRFIDScanTime = 0;

// ============================
// Variables
// ============================
bool counting[4] = {false, false, false, false};
unsigned long startTime[4] = {0, 0, 0, 0};
int paymentDue[4] = {0, 0, 0, 0};
int coinsInserted[4] = {0, 0, 0, 0};
int currentPayingIndex = -1;
char slotStatus[4] = {'O', 'O', 'O', 'O'};
bool awaitingPayment[4] = {false, false, false, false};
bool reservedStatus[4] = {false, false, false, false};
int capacityHold = 0;

// ============================
// Card dispense queue
// ============================
int availableQueue[4] = {0, 1, 2, 3};
int queueHead = 0;
int queueSize = 4;

int popNextCardFromQueue() {
  if (queueSize == 0) return -1;
  int idx = availableQueue[queueHead];
  queueHead = (queueHead + 1) % 4;
  queueSize--;
  return idx;
}

void pushReturnedCardToQueue(int idx) {
  if (queueSize >= 4) return;
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
  for (int k = 0; k < newSize; k++) {
    availableQueue[k] = newQueue[k];
  }
  queueHead = 0;
  queueSize = newSize;
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
// Serial Commands
// ============================
void readHostCommands() {
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    
    if (line.startsWith("RESV:")) {
      String payload = line.substring(5);
      Serial.print(F("Received RESV: "));
      Serial.println(payload);
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
    } 
    else if (line.startsWith("START:")) {
      String idxStr = line.substring(6);
      idxStr.trim();
      int oneBased = idxStr.toInt();
      if (oneBased >= 1 && oneBased <= 4) {
        int i = oneBased - 1;
        if (!counting[i] && !awaitingPayment[i]) {
          counting[i] = true;
          startTime[i] = millis();
          removeCardFromQueueIfPresent(i);
          Serial.print(F("Started timer for Card "));
          Serial.println(i + 1);
        }
      }
    } 
    else if (line.startsWith("HERE:")) {
      String idxStr = line.substring(5);
      idxStr.trim();
      int oneBased = idxStr.toInt();
      Serial.print(F("HERE command for slot "));
      Serial.println(oneBased);
      
      if (oneBased >= 1 && oneBased <= 4) {
        int allowedAvailable = queueSize - capacityHold;
        if (allowedAvailable <= 0) {
          Serial.println(F("No available card/slot (capacity held) for reserved user."));
        } else {
          // First try to find an unreserved card
          int nextCard = -1;
          for (int attempt = 0; attempt < queueSize; attempt++) {
            int pos = (queueHead + attempt) % 4;
            int candidate = availableQueue[pos];
            if (!reservedStatus[candidate] && !counting[candidate] && !awaitingPayment[candidate]) {
              nextCard = candidate;
              // Remove from queue
              for (int k = attempt; k < queueSize - 1; k++) {
                int curPos = (queueHead + k) % 4;
                int nextPos = (queueHead + k + 1) % 4;
                availableQueue[curPos] = availableQueue[nextPos];
              }
              queueSize--;
              break;
            }
          }

          if (nextCard != -1) {
            counting[nextCard] = true;
            startTime[nextCard] = millis();

            Serial.print(F("Assigned next card "));
            Serial.print(nextCard + 1);
            Serial.println(F(" to reserved user"));
            
            Serial.print(F("DISP:"));
            Serial.println(nextCard + 1);

            Serial.println(F("Dispensing card and opening gate..."));
            dispenserServo.write(90);
            delay(700);
            dispenserServo.write(0);

            entryServo.write(90);
            delay(5000);
            entryServo.write(0);
            Serial.println(F("Gate closed."));
          } else {
            Serial.println(F("No available card/slot for reserved user"));
          }
        }
      }
    } 
    else if (line.startsWith("CAP:")) {
      String num = line.substring(4);
      num.trim();
      int n = num.toInt();
      if (n < 0) n = 0;
      if (n > 4) n = 4;
      capacityHold = n;
      Serial.print(F("Capacity hold set to "));
      Serial.println(capacityHold);
    } 
    else if (line.startsWith("RFID:")) {
      String payload = line.substring(5);
      Serial.print(F("Received VIP RFID data: "));
      Serial.println(payload);
      
      numReservedRFIDs = 0;
      
      int start = 0;
      int comma = payload.indexOf(',');
      while (comma != -1 && numReservedRFIDs < 10) {
        String pair = payload.substring(start, comma);
        pair.trim();
        if (pair.length() > 0) {
          int colon = pair.indexOf(':');
          if (colon != -1) {
            String rfid = pair.substring(0, colon);
            String email = pair.substring(colon + 1);
            rfid.trim();
            email.trim();
            if (rfid.length() > 0 && email.length() > 0) {
              reservedRFIDs[numReservedRFIDs] = rfid;
              reservedCustomers[numReservedRFIDs] = email;
              numReservedRFIDs++;
            }
          }
        }
        start = comma + 1;
        comma = payload.indexOf(',', start);
      }
      
      if (start < payload.length() && numReservedRFIDs < 10) {
        String pair = payload.substring(start);
        pair.trim();
        if (pair.length() > 0) {
          int colon = pair.indexOf(':');
          if (colon != -1) {
            String rfid = pair.substring(0, colon);
            String email = pair.substring(colon + 1);
            rfid.trim();
            email.trim();
            if (rfid.length() > 0 && email.length() > 0) {
              reservedRFIDs[numReservedRFIDs] = rfid;
              reservedCustomers[numReservedRFIDs] = email;
              numReservedRFIDs++;
            }
          }
        }
      }
      
      Serial.print(F("Loaded "));
      Serial.print(numReservedRFIDs);
      Serial.println(F(" VIP RFID cards"));
    }
  }
}

// ============================
// LCD Update Function
// ============================
void updateLCD() {
  lcd.clear();
  
  // If someone is paying, show payment info
  if (currentPayingIndex != -1) {
    int idx = currentPayingIndex;
    int coinsNeeded = paymentDue[idx] / 5;
    int remaining = (coinsNeeded - coinsInserted[idx]) * 5;
    if (remaining < 0) remaining = 0;
    
    lcd.setCursor(0, 0);
    lcd.print(F("Card "));
    lcd.print(idx + 1);
    lcd.print(F(" Payment"));
    
    lcd.setCursor(0, 1);
    lcd.print(F("Pay: Php "));
    lcd.print(remaining);
  } else {
    // Show slot status
    lcd.setCursor(0, 0);
    lcd.print(F("S1:")); lcd.print(slotStatus[0]);
    lcd.print(F(" S2:")); lcd.print(slotStatus[1]);
    lcd.setCursor(0, 1);
    lcd.print(F("S3:")); lcd.print(slotStatus[2]);
    lcd.print(F(" S4:")); lcd.print(slotStatus[3]);
  }
}

// ============================
// Setup
// ============================
void setup() {
  Serial.begin(9600);
  Serial.setTimeout(50);
  while (!Serial) {
    ;
  }
  Serial.println(F("System Starting..."));

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinInserted, FALLING);

  pinMode(TRIG1, OUTPUT); pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT); pinMode(ECHO2, INPUT);
  pinMode(TRIG3, OUTPUT); pinMode(ECHO3, INPUT);
  pinMode(TRIG4, OUTPUT); pinMode(ECHO4, INPUT);

  entryServo.attach(ENTRY_SERVO_PIN);
  exitServo.attach(EXIT_SERVO_PIN);
  dispenserServo.attach(DISPENSER_SERVO_PIN);
  entryServo.write(0);
  exitServo.write(0);
  dispenserServo.write(0);

  Wire.begin();
  Wire.setClock(100000);

  Serial.println(F("I2C initialized"));

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print(F("Initializing..."));
  Serial.println(F("LCD initialized"));

  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    lcd.clear();
    lcd.print(F("PN532 not found!"));
    Serial.println(F("PN532 not detected!"));
    while (1);
  }
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(0x10); // Faster detection
  Serial.println(F("PN532 initialized"));

  lcd.clear();
  lcd.print(F("System Ready"));
  Serial.println(F("System Ready"));
  delay(1000);
}

// ============================
// RFID Helper Functions
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

String uidToString(uint8_t* uid, uint8_t uidLength) {
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";
    uidStr += String(uid[i], HEX);
  }
  uidStr.toUpperCase();
  return uidStr;
}

int getReservedCustomerIndex(String rfid) {
  for (int i = 0; i < numReservedRFIDs; i++) {
    if (reservedRFIDs[i] == rfid) {
      return i;
    }
  }
  return -1;
}

void handleVIPRFIDScan(String rfid) {
  int customerIndex = getReservedCustomerIndex(rfid);
  
  if (customerIndex != -1) {
      String customerEmail = reservedCustomers[customerIndex];
      Serial.print(F("VIP RFID: Customer detected - "));
      Serial.println(customerEmail);
      Serial.print(F("VIP RFID: Card UID - "));
      Serial.println(rfid);
      
      int allowedAvailable = queueSize - capacityHold;
      if (allowedAvailable <= 0) {
          Serial.println(F("VIP RFID: No available card/slot (capacity held)"));
          return;
      }

      // Find next unreserved card
      int nextCard = -1;
      for (int attempt = 0; attempt < queueSize; attempt++) {
        int pos = (queueHead + attempt) % 4;
        int candidate = availableQueue[pos];
        if (!reservedStatus[candidate] && !counting[candidate] && !awaitingPayment[candidate]) {
          nextCard = candidate;
          // Remove from queue
          for (int k = attempt; k < queueSize - 1; k++) {
            int curPos = (queueHead + k) % 4;
            int nextPos = (queueHead + k + 1) % 4;
            availableQueue[curPos] = availableQueue[nextPos];
          }
          queueSize--;
          break;
        }
      }

      if (nextCard != -1) {
          // Start counting immediately for VIP users
          counting[nextCard] = true;
          startTime[nextCard] = millis();
          
          Serial.print(F("VIP RFID: Dispensed Card "));
          Serial.println(nextCard + 1);

          Serial.print(F("DISP:"));
          Serial.println(nextCard + 1);

          Serial.println(F("VIP RFID: Dispensing card..."));
          dispenserServo.write(90);
          delay(700);
          dispenserServo.write(0);

          Serial.println(F("VIP RFID: Opening gate..."));
          entryServo.write(90);
          delay(5000);
          entryServo.write(0);
          Serial.println(F("VIP RFID: Gate closed - Service complete"));
      } else {
          Serial.println(F("VIP RFID: No available card/slot"));
      }
  }
}

// ============================
// Main Loop
// ============================
void loop() {
  readHostCommands();
  
  long d1 = getDistance(TRIG1, ECHO1);
  long d2 = getDistance(TRIG2, ECHO2);
  long d3 = getDistance(TRIG3, ECHO3);
  long d4 = getDistance(TRIG4, ECHO4);

  bool occ0 = (d1 < 15);
  bool occ1 = (d2 < 15);
  bool occ2 = (d3 < 15);
  bool occ3 = (d4 < 15);

  slotStatus[0] = occ0 ? 'X' : ((counting[0] || awaitingPayment[0] || reservedStatus[0]) ? 'R' : 'O');
  slotStatus[1] = occ1 ? 'X' : ((counting[1] || awaitingPayment[1] || reservedStatus[1]) ? 'R' : 'O');
  slotStatus[2] = occ2 ? 'X' : ((counting[2] || awaitingPayment[2] || reservedStatus[2]) ? 'R' : 'O');
  slotStatus[3] = occ3 ? 'X' : ((counting[3] || awaitingPayment[3] || reservedStatus[3]) ? 'R' : 'O');

  updateLCD();

  Serial.print(F("SLOTS:"));
  Serial.print(slotStatus[0]);
  Serial.print(",");
  Serial.print(slotStatus[1]);
  Serial.print(",");
  Serial.print(slotStatus[2]);
  Serial.print(",");
  Serial.println(slotStatus[3]);
  Serial.flush();

  // ENTRY BUTTON - Now works with reservations
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(200);
    Serial.println(F("Button pressed"));

    int allowedAvailable = queueSize - capacityHold;
    if (allowedAvailable <= 0) {
      Serial.println(F("No available card/slot"));
      delay(2000);
    } else {
      // Find next unreserved available card
      int nextCard = -1;
      for (int attempt = 0; attempt < queueSize; attempt++) {
        int pos = (queueHead + attempt) % 4;
        int candidate = availableQueue[pos];
        if (!reservedStatus[candidate] && !counting[candidate] && !awaitingPayment[candidate]) {
          nextCard = candidate;
          // Remove from queue
          for (int k = attempt; k < queueSize - 1; k++) {
            int curPos = (queueHead + k) % 4;
            int nextPos = (queueHead + k + 1) % 4;
            availableQueue[curPos] = availableQueue[nextPos];
          }
          queueSize--;
          break;
        }
      }
      
      if (nextCard != -1) {
        counting[nextCard] = true;
        startTime[nextCard] = millis();

        Serial.print(F("Dispensed Card "));
        Serial.println(nextCard + 1);
        
        Serial.print(F("DISP:"));
        Serial.println(nextCard + 1);

        dispenserServo.write(90);
        delay(700);
        dispenserServo.write(0);

        entryServo.write(90);
        delay(5000);
        entryServo.write(0);
        Serial.println(F("Entry gate closed"));

        delay(1000);
        return;
      } else {
        Serial.println(F("No available card/slot"));
        delay(2000);
      }
    }
  }

  // RFID SCAN - Priority: parking cards first, then VIP badges
  uint8_t success;
  uint8_t uid[7];
  uint8_t uidLength;
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50);

  if (success) {
    String rfidString = uidToString(uid, uidLength);
    
    // ALWAYS check parking cards first (for exit)
    int cardIndex = matchCard(uid);
    
    if (cardIndex != -1) {
      // This is a parking card
      if (counting[cardIndex]) {
        // Customer is exiting - calculate payment
        unsigned long totalTime = (millis() - startTime[cardIndex]) / 1000;
        counting[cardIndex] = false;

        paymentDue[cardIndex] = ((totalTime + 4) / 5) * 5;
        coinsInserted[cardIndex] = 0;
        awaitingPayment[cardIndex] = true;

        Serial.print(F("Card "));
        Serial.print(cardIndex + 1);
        Serial.print(F(" stayed "));
        Serial.print(totalTime);
        Serial.print(F("s -> Pay "));
        Serial.print(paymentDue[cardIndex]);
        Serial.println(F(" PHP"));

        if (currentPayingIndex == -1) {
          currentPayingIndex = cardIndex;
        }
        
        delay(500);
        return;
      }
    } else {
      // Not a parking card - check if VIP badge
      if (rfidString != lastScannedRFID || (millis() - lastRFIDScanTime) > 2000) {
        lastScannedRFID = rfidString;
        lastRFIDScanTime = millis();
        
        if (getReservedCustomerIndex(rfidString) != -1) {
          handleVIPRFIDScan(rfidString);
          delay(500);
          return;
        }
      }
    }
  }

  // PAYMENT PROCESSING
  if (currentPayingIndex != -1) {
    int idx = currentPayingIndex;
    int coinsNeeded = paymentDue[idx] / 5;

    if (coinsInserted[idx] >= coinsNeeded) {
      // Show thank you message on LCD
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F("Thank You!"));
      lcd.setCursor(0, 1);
      lcd.print(F("Payment Complete"));
      
      delay(2000);

      Serial.print(F("Card "));
      Serial.print(idx + 1);
      Serial.println(F(" payment complete"));

      exitServo.write(90);
      delay(5000);
      exitServo.write(0);
      Serial.println(F("Exit gate closed"));

      awaitingPayment[idx] = false;
      paymentDue[idx] = 0;
      coinsInserted[idx] = 0;
      currentPayingIndex = -1;

      pushReturnedCardToQueue(idx);

      Serial.print(F("PAID:"));
      Serial.println(idx + 1);

      // Check if another card is waiting for payment
      for (int i = 0; i < numCards; i++) {
        if (awaitingPayment[i]) {
          currentPayingIndex = i;
          break;
        }
      }
      
      // Update LCD to show slots again
      updateLCD();
    }
  }

  delay(50);
}