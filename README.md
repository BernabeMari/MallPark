# Automated Mall Parking System

This project implements an automated mall parking system using Arduino with RFID card detection via PN532 (I²C) and a Flask web interface for monitoring and control.

## Features

- **RFID Card Detection**: Uses PN532 NFC/RFID reader to detect 4 predefined parking cards
- **Button-Triggered Registration**: Press a physical button to register the first available card as a parking ticket
- **Real-time Web Monitoring**: Flask web interface shows live parking status and statistics
- **Card Management**: Track which cards are registered and when they were registered
- **System Reset**: Reset all parking registrations through the web interface

## Hardware Requirements

- Arduino Uno/Nano (or compatible)
- PN532 NFC/RFID Module (I²C interface)
- Push button
- LED (for status indication)
- 4 RFID cards with addresses:
  - `35 E1 7A D3`
  - `65 07 D5 D3`
  - `55 C9 5C D3`
  - `35 33 3F D3`

## Wiring Diagram

```
Arduino    PN532
--------   ------
5V    →    VCC
GND   →    GND
A4    →    SDA
A5    →    SCL

Arduino    Button    LED
--------   ------    ---
Pin 4  →   One side
GND    →   Other side (with pullup)
Pin 5  →   Anode (+)
GND    →   Cathode (-)
```

## Setup Instructions

### 1. Arduino Setup
1. Install the Adafruit PN532 library:
   - Open Arduino IDE
   - Go to Tools → Manage Libraries
   - Search for "Adafruit PN532" and install it
2. Connect your hardware according to the wiring diagram
3. Upload the `arduino_serial_hello.ino` code to your Arduino
4. Open Serial Monitor (Tools → Serial Monitor) and set baud rate to 115200
5. Note the COM port (e.g., COM3, COM4) that appears in the Serial Monitor

### 2. Python Setup
1. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```

2. Run the Flask app:
   ```bash
   python app.py
   ```

3. Open browser: http://localhost:5000

### 3. Using the System

#### Physical Operation:
1. Place RFID cards near the PN532 reader
2. Press the physical button on Arduino to register the first available card
3. LED will blink to indicate successful registration
4. Check Serial Monitor for detailed system status

#### Web Interface:
1. Click "Connect" to connect to your Arduino
2. Monitor real-time parking status:
   - Total cards: 4
   - Available cards (green)
   - Registered cards (red)
   - Occupancy percentage
3. Use "Reset All Registrations" to clear all parking tickets
4. View system log for detailed activity

## System Behavior

- **Card Detection**: System continuously scans for RFID cards and identifies them
- **Registration Logic**: When button is pressed, system finds the first available card and registers it
- **Status Tracking**: Each card's registration status and timestamp are tracked
- **Error Handling**: System handles cases where no cards are available for registration
- **Real-time Updates**: Web interface updates every 2 seconds when connected

## Files

- `arduino_serial_hello.ino` - Arduino code for parking system
- `app.py` - Flask web application with parking system integration
- `templates/index.html` - Web interface for parking system monitoring
- `requirements.txt` - Python dependencies
- `README.md` - This documentation

## Troubleshooting

- **Arduino not detected**: Check USB connection and install proper drivers
- **PN532 not working**: Verify I²C connections and power supply
- **Cards not detected**: Ensure cards are close enough to reader and are the correct type
- **Web interface not updating**: Check Arduino connection and serial communication
- **Button not responding**: Check button wiring and debounce settings

## Technical Details

- **Communication**: Serial communication at 115200 baud
- **RFID Protocol**: ISO14443A (MIFARE compatible)
- **Card Format**: 4-byte UID cards
- **Web Framework**: Flask with real-time updates
- **Data Format**: JSON for web communication

## Future Enhancements

- Add parking duration tracking
- Implement parking fees calculation
- Add card validation and security features
- Support for more RFID card types
- Database storage for parking history
- Mobile app integration
