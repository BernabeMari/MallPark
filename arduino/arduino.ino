#include <Wire.h>
#include <Adafruit_PN532.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <Tiny4kOLED.h>

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

// Track which VIP badge is using which slot
String slotVIPCard[4] = {"", "", "", ""};

// ============================
// Variables
// ============================
bool counting[4] = {false, false, false, false};
unsigned long startTime[4] = {0, 0, 0, 0};
int paymentDue[4] = {0, 0, 0, 0};
int coinsInserted[4] = {0, 0, 0, 0};
int currentPayingIndex = -1;
char slotStatus[4] = {'O', 'O', 'O', 'O'};
bool oledAvailable = false;
bool awaitingPayment[4] = {false, false, false, false};
bool reservedStatus[4] = {false, false, false, false};
int capacityHold = 0;
bool isWalkInCustomer[4] = {false, false, false, false}; // Track if slot is used by walk-in during cap hold

// ============================
// Card dispense queue
// ============================
int availableQueue[4] = {0, 1, 2, 3};
int queueHead = 0;
int queueSize = 4;

// ============================
// Debounce tracking
// ============================
unsigned long lastButtonPress = 0;
unsigned long lastRFIDProcess = 0;
const unsigned long DEBOUNCE_DELAY = 1000;

// ============================
// Non-blocking servo control
// ============================
enum ServoState {
  SERVO_IDLE,
  DISPENSER_ACTIVE,
  ENTRY_GATE_OPENING,
  ENTRY_GATE_OPEN,
  EXIT_GATE_OPENING,
  EXIT_GATE_OPEN,
  PAYMENT_DISPLAY
};

ServoState currentServoState = SERVO_IDLE;
unsigned long servoStateStartTime = 0;
int pendingCardDispense = -1;

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
// Non-blocking servo state machine
// ============================
void updateServoStateMachine() {
  unsigned long elapsed = millis() - servoStateStartTime;
  
  switch (currentServoState) {
    case SERVO_IDLE:
      // Do nothing, waiting for next action
      break;
      
    case DISPENSER_ACTIVE:
      if (elapsed >= 700) {
        dispenserServo.write(0);
        Serial.println(F("Dispenser complete"));
        
        // Move to entry gate opening state
        currentServoState = ENTRY_GATE_OPENING;
        servoStateStartTime = millis();
        Serial.println(F("Preparing to open entry gate..."));
      }
      break;
      
    case ENTRY_GATE_OPENING:
      if (elapsed >= 200) {
        entryServo.write(90);
        Serial.println(F("Entry gate servo activated"));
        currentServoState = ENTRY_GATE_OPEN;
        servoStateStartTime = millis();
      }
      break;
      
    case ENTRY_GATE_OPEN:
      if (elapsed >= 5000) {
        entryServo.write(0);
        Serial.println(F("Entry gate closed"));
        currentServoState = SERVO_IDLE;
      }
      break;
      
    case EXIT_GATE_OPENING:
      if (elapsed >= 200) {
        exitServo.write(90);
        Serial.println(F("Exit gate servo activated"));
        currentServoState = EXIT_GATE_OPEN;
        servoStateStartTime = millis();
      }
      break;
      
    case EXIT_GATE_OPEN:
      if (elapsed >= 5000) {
        exitServo.write(0);
        Serial.println(F("Exit gate closed"));
        
        int idx = pendingCardDispense;
        if (idx != -1) {
          awaitingPayment[idx] = false;
          paymentDue[idx] = 0;
          coinsInserted[idx] = 0;
          currentPayingIndex = -1;

          pushReturnedCardToQueue(idx);
          
          // Clear walk-in flag
          isWalkInCustomer[idx] = false;

          Serial.print(F("PAID:"));
          Serial.println(idx + 1);

          for (int i = 0; i < numCards; i++) {
            if (awaitingPayment[i]) {
              currentPayingIndex = i;
              break;
            }
          }
          pendingCardDispense = -1;
        }
        currentServoState = SERVO_IDLE;
      }
      break;
      
    case PAYMENT_DISPLAY:
      if (elapsed >= 2000) {
        // After payment display, prepare to open exit gate
        currentServoState = EXIT_GATE_OPENING;
        servoStateStartTime = millis();
        Serial.println(F("Preparing to open exit gate..."));
      }
      break;
  }
}

// ============================
// Start card dispense sequence
// ============================
void startCardDispense(int cardIndex) {
  if (currentServoState != SERVO_IDLE) {
    Serial.println(F("Servo busy, cannot dispense"));
    return;
  }
  
  Serial.print(F("Dispensed Card "));
  Serial.println(cardIndex + 1);
  
  Serial.print(F("DISP:"));
  Serial.println(cardIndex + 1);
  
  currentServoState = DISPENSER_ACTIVE;
  servoStateStartTime = millis();
  dispenserServo.write(90);
  Serial.println(F("Dispensing card..."));
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
        
        bool wasReserved = reservedStatus[i];
        bool nowReserved = (token == "R");
        reservedStatus[i] = nowReserved;
        
        if (!wasReserved && nowReserved && !counting[i] && !awaitingPayment[i]) {
          removeCardFromQueueIfPresent(i);
          Serial.print(F("Removed card "));
          Serial.print(i + 1);
          Serial.println(F(" from queue (reserved)"));
        }
        
        if (wasReserved && !nowReserved && !counting[i] && !awaitingPayment[i]) {
          pushReturnedCardToQueue(i);
          Serial.print(F("Added card "));
          Serial.print(i + 1);
          Serial.println(F(" back to queue (unreserved)"));
        }
        
        if (comma == -1) break;
        payload.remove(0, comma + 1);
      }
      
      Serial.print(F("Queue size: "));
      Serial.print(queueSize);
      Serial.print(F(" | Cards in queue: "));
      for (int k = 0; k < queueSize; k++) {
        int pos = (queueHead + k) % 4;
        Serial.print(availableQueue[pos] + 1);
        Serial.print(" ");
      }
      Serial.println();
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
          int nextCard = -1;
          for (int attempt = 0; attempt < queueSize; attempt++) {
            int pos = (queueHead + attempt) % 4;
            int candidate = availableQueue[pos];
            if (!reservedStatus[candidate] && !counting[candidate] && !awaitingPayment[candidate]) {
              nextCard = candidate;
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
            
            startCardDispense(nextCard);
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
// OLED Update Function
// ============================
void updateOLED() {
  if (!oledAvailable) return;

  oled.clear();
  oled.setFont(FONT8X16);

  if (currentPayingIndex != -1) {
    int idx = currentPayingIndex;
    int coinsNeeded = paymentDue[idx] / 5;
    int remaining = (coinsNeeded - coinsInserted[idx]) * 5;
    if (remaining < 0) remaining = 0;

    oled.setCursor(0, 0);
    oled.print(F("Slot "));
    oled.print(idx + 1);
    oled.print(F(" Payment"));

    oled.setCursor(0, 2);
    oled.print(F("Php "));
    oled.print(remaining);
  } else {
    oled.setCursor(0, 0);
    oled.print(F("Welcome to"));
    oled.setCursor(0, 2);
    oled.print(F("Parking System"));
  }
}

// ============================
// LCD Update Function
// ============================
void updateLCD() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("S1:")); lcd.print(slotStatus[0]);
  lcd.print(F(" S2:")); lcd.print(slotStatus[1]);
  lcd.setCursor(0, 1);
  lcd.print(F("S3:")); lcd.print(slotStatus[2]);
  lcd.print(F(" S4:")); lcd.print(slotStatus[3]);
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

  oled.begin();
  oled.clear();
  oled.on();
  oledAvailable = true;
  Serial.println(F("OLED initialized"));

  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    lcd.clear();
    lcd.print(F("PN532 not found!"));
    Serial.println(F("PN532 not detected!"));
    while (1);
  }
  nfc.SAMConfig();
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

int findSlotByVIPCard(String rfid) {
  for (int i = 0; i < 4; i++) {
    if (slotVIPCard[i] == rfid) {
      return i;
    }
  }
  return -1;
}

void handleVIPRFIDScan(String rfid) {
  int customerIndex = getReservedCustomerIndex(rfid);
  
  if (customerIndex != -1) {
    int assignedSlot = findSlotByVIPCard(rfid);
    
    if (assignedSlot != -1 && counting[assignedSlot]) {
      Serial.print(F("VIP RFID: Exit detected for slot "));
      Serial.println(assignedSlot + 1);
      
      unsigned long totalTime = (millis() - startTime[assignedSlot]) / 1000;
      counting[assignedSlot] = false;

      paymentDue[assignedSlot] = ((totalTime + 4) / 5) * 5;
      coinsInserted[assignedSlot] = 0;
      awaitingPayment[assignedSlot] = true;

      Serial.print(F("Slot "));
      Serial.print(assignedSlot + 1);
      Serial.print(F(" stayed "));
      Serial.print(totalTime);
      Serial.print(F("s -> Pay "));
      Serial.print(paymentDue[assignedSlot]);
      Serial.println(F(" PHP"));

      if (currentPayingIndex == -1) {
        currentPayingIndex = assignedSlot;
      }
      
      slotVIPCard[assignedSlot] = "";
      isWalkInCustomer[assignedSlot] = false;
      lastRFIDProcess = millis();
      return;
    }
    
    String customerEmail = reservedCustomers[customerIndex];
    Serial.print(F("VIP RFID: Customer detected - "));
    Serial.println(customerEmail);
    Serial.print(F("VIP RFID: Card UID - "));
    Serial.println(rfid);
    
    Serial.print(F("Queue size: "));
    Serial.print(queueSize);
    Serial.print(F(", Capacity hold: "));
    Serial.println(capacityHold);

    int nextCard = -1;
    
    // VIP customer gets next card from queue (bypasses capacity hold)
    for (int attempt = 0; attempt < queueSize; attempt++) {
      int pos = (queueHead + attempt) % 4;
      int candidate = availableQueue[pos];
      if (!reservedStatus[candidate] && !counting[candidate] && !awaitingPayment[candidate]) {
        nextCard = candidate;
        for (int k = attempt; k < queueSize - 1; k++) {
          int curPos = (queueHead + k) % 4;
          int nextPos = (queueHead + k + 1) % 4;
          availableQueue[curPos] = availableQueue[nextPos];
        }
        queueSize--;
        Serial.println(F("VIP getting reserved card"));
        break;
      }
    }
    
    // If no cards in queue, check for any physically available slot
    if (nextCard == -1) {
      Serial.println(F("Queue empty, checking for physically available slots..."));
      for (int i = 0; i < 4; i++) {
        if (!reservedStatus[i] && !counting[i] && !awaitingPayment[i]) {
          nextCard = i;
          removeCardFromQueueIfPresent(i);
          Serial.print(F("VIP taking physically available slot "));
          Serial.println(i + 1);
          break;
        }
      }
    }

    if (nextCard != -1) {
      counting[nextCard] = true;
      startTime[nextCard] = millis();
      isWalkInCustomer[nextCard] = false;

      slotVIPCard[nextCard] = rfid;
      
      Serial.print(F("VIP RFID: Dispensed Card "));
      Serial.print(nextCard + 1);
      Serial.print(F(" - Linked to VIP badge"));
      Serial.println();

      startCardDispense(nextCard);
      
      lastRFIDProcess = millis();
    } else {
      Serial.println(F("VIP RFID: All slots physically occupied. Please wait."));
    }
  }
}

// ============================
// Main Loop
// ============================
void loop() {
  // Update servo state machine (non-blocking)
  updateServoStateMachine();
  
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

  updateOLED();
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

  // ENTRY BUTTON with debounce
  if (digitalRead(BUTTON_PIN) == LOW && (millis() - lastButtonPress) > DEBOUNCE_DELAY) {
    lastButtonPress = millis();
    Serial.println(F("Button pressed"));

    int allowedAvailable = queueSize - capacityHold;
    
    if (allowedAvailable <= 0) {
      Serial.print(F("Walk-in DENIED - No available slots"));
      Serial.print(F(" (Queue: "));
      Serial.print(queueSize);
      Serial.print(F(", Reserved: "));
      Serial.print(capacityHold);
      Serial.println(F(")"));
      Serial.println(F("Slots are reserved for upcoming customers."));
      // Do not dispense anything - walk-in is rejected
    } else {
      // Walk-in is allowed - get next card from queue
      int nextCard = -1;
      for (int attempt = 0; attempt < queueSize; attempt++) {
        int pos = (queueHead + attempt) % 4;
        int candidate = availableQueue[pos];
        if (!reservedStatus[candidate] && !counting[candidate] && !awaitingPayment[candidate]) {
          nextCard = candidate;
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
        isWalkInCustomer[nextCard] = false;
        
        Serial.print(F("Walk-in dispensed Card "));
        Serial.println(nextCard + 1);
        
        startCardDispense(nextCard);
      } else {
        Serial.println(F("No available card/slot"));
      }
    }
  }

  // RFID SCAN with debounce
  if ((millis() - lastRFIDProcess) > DEBOUNCE_DELAY) {
    uint8_t success;
    uint8_t uid[7];
    uint8_t uidLength;
    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100);

    if (success) {
      String rfidString = uidToString(uid, uidLength);
      
      int cardIndex = matchCard(uid);
      
      if (cardIndex != -1) {
        if (counting[cardIndex]) {
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
          
          slotVIPCard[cardIndex] = "";
          isWalkInCustomer[cardIndex] = false;
          lastRFIDProcess = millis();
        }
      } else {
        if (rfidString != lastScannedRFID || (millis() - lastRFIDScanTime) > 2000) {
          lastScannedRFID = rfidString;
          lastRFIDScanTime = millis();
          
          if (getReservedCustomerIndex(rfidString) != -1) {
            handleVIPRFIDScan(rfidString);
          }
        }
      }
    }
  }

  // PAYMENT PROCESSING
  if (currentPayingIndex != -1 && currentServoState == SERVO_IDLE) {
    int idx = currentPayingIndex;
    int coinsNeeded = paymentDue[idx] / 5;

    if (coinsInserted[idx] >= coinsNeeded) {
      oled.clear();
      oled.setFont(FONT8X16);
      oled.setCursor(0, 0);
      oled.print(F("Thank You!"));
      oled.setCursor(0, 2);
      oled.print(F("Payment OK"));
      
      Serial.print(F("Card "));
      Serial.print(idx + 1);
      Serial.println(F(" payment complete"));

      // Start non-blocking payment sequence
      pendingCardDispense = idx;
      currentServoState = PAYMENT_DISPLAY;
      servoStateStartTime = millis();
    }
  }

  delay(100);
}