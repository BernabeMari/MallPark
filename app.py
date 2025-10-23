from flask import Flask, render_template, request, jsonify, redirect, url_for, session, send_from_directory
from flask_mail import Mail, Message
from functools import wraps
import serial
import serial.tools.list_ports
import threading
import time
import random
import string
import csv
import os
from datetime import datetime

app = Flask(__name__)
app.secret_key = "change-this-secret-key"

# Email configuration
app.config['MAIL_SERVER'] = 'smtp.gmail.com'
app.config['MAIL_PORT'] = 587
app.config['MAIL_USE_TLS'] = True
app.config['MAIL_USERNAME'] = 'spradax20@gmail.com'  # Change this
app.config['MAIL_PASSWORD'] = 'tzro xfyy fyvj nnwz'     # Change this
mail = Mail(app)

# CSV Database files
CUSTOMERS_CSV = "data/customers.csv"
VERIFICATION_CSV = "data/verification_codes.csv"
PENDING_CSV = "data/pending_accounts.csv"

# Create data directory if it doesn't exist
os.makedirs("data", exist_ok=True)

# In-memory users and verification codes
USERS = {
    "admin": "admin123"  # Default admin account
}

# Parking spots state: O=open, X=occupied, R=reserved
# For demo, start all open. Integrate with Arduino later for live X state.
spots = [
    {"id": 1, "state": "O", "reserved_by": None, "reserved_at": None, "arrival_ts": None, "assigned_slot": None},
    {"id": 2, "state": "O", "reserved_by": None, "reserved_at": None, "arrival_ts": None, "assigned_slot": None},
    {"id": 3, "state": "O", "reserved_by": None, "reserved_at": None, "arrival_ts": None, "assigned_slot": None},
    {"id": 4, "state": "O", "reserved_by": None, "reserved_at": None, "arrival_ts": None, "assigned_slot": None},
]

# Server-side card queue to match Arduino round-robin behavior
# This mirrors the Arduino's availableQueue[4] = {0, 1, 2, 3} and queueHead = 0, queueSize = 4
server_card_queue = [0, 1, 2, 3]  # Card indices 0-3 (1-4 in 1-based)
server_queue_head = 0
server_queue_size = 4

# Arduino serial connection
arduino_serial = None
last_resv_line = None
last_cap_sent = None

def send_command_to_arduino(command, description="command"):
    """Safe wrapper for sending commands to Arduino with proper error handling"""
    global arduino_serial
    if not (arduino_serial and arduino_serial.is_open):
        print(f"Arduino not connected - cannot send {description}")
        return False
    
    try:
        # Clear buffers before sending
        arduino_serial.reset_input_buffer()
        arduino_serial.reset_output_buffer()
        
        # Send command
        arduino_serial.write(command.encode())
        arduino_serial.flush()  # Force send immediately
        
        print(f"{description} sent successfully: {command.strip()}")
        time.sleep(0.15)  # Wait 150ms between commands
        return True
    except serial.SerialTimeoutException:
        print(f"Timeout sending {description}: {command.strip()}")
        return False
    except Exception as e:
        print(f"Error sending {description}: {e}")
        return False

def find_arduino_port():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        if any(x in p.description.lower() for x in ["arduino", "ch340", "cp210", "ftdi", "usb serial"]):
            return p.device
    return None


# Server-side card queue management (mirrors Arduino behavior)
def pop_next_card_from_queue():
    """Pop next card from server queue (mirrors Arduino popNextCardFromQueue)"""
    global server_queue_head, server_queue_size
    if server_queue_size == 0:
        return -1
    card_idx = server_card_queue[server_queue_head]
    server_queue_head = (server_queue_head + 1) % 4
    server_queue_size -= 1
    return card_idx


def push_returned_card_to_queue(card_idx):
    """Push returned card to end of server queue (mirrors Arduino pushReturnedCardToQueue)"""
    global server_queue_head, server_queue_size
    if server_queue_size >= 4:
        return  # should not happen
    insert_pos = (server_queue_head + server_queue_size) % 4
    server_card_queue[insert_pos] = card_idx
    server_queue_size += 1


def remove_card_from_queue_if_present(card_idx):
    """Remove card from server queue if present (mirrors Arduino removeCardFromQueueIfPresent)"""
    global server_queue_head, server_queue_size
    if server_queue_size == 0:
        return
    new_queue = []
    for i in range(server_queue_size):
        pos = (server_queue_head + i) % 4
        val = server_card_queue[pos]
        if val != card_idx:
            new_queue.append(val)
    # Write back
    for i in range(len(new_queue)):
        server_card_queue[i] = new_queue[i]
    server_queue_head = 0
    server_queue_size = len(new_queue)


def connect_arduino(port=None, baudrate=9600):
    global arduino_serial
    try:
        if port is None:
            port = find_arduino_port()
        if not port:
            return False, "No Arduino found"
        
        arduino_serial = serial.Serial(
            port=port,
            baudrate=baudrate,
            timeout=1,
            write_timeout=2,  # Add write timeout
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE
        )
        
        time.sleep(2)  # Wait for Arduino to reset
        
        # Clear any pending data
        arduino_serial.reset_input_buffer()
        arduino_serial.reset_output_buffer()
        
        threading.Thread(target=read_arduino_loop, daemon=True).start()
        return True, f"Connected: {port}"
    except Exception as e:
        return False, str(e)


def read_arduino_loop():
    global arduino_serial, spots
    while arduino_serial and arduino_serial.is_open:
        try:
            # Enforce 10-minute hold policy periodically
            enforce_holds()
            if arduino_serial.in_waiting > 0:
                line = arduino_serial.readline().decode(errors='ignore').strip()
                if line.startswith("SLOTS:"):
                    payload = line.split(":", 1)[1]
                    parts = [x.strip() for x in payload.split(",")]
                    for i in range(min(4, len(parts))):
                        # Device-reported states: X=occupied, R=reserved/session-active, O=open
                        if parts[i] == 'X':
                            spots[i]["state"] = 'X'
                        elif parts[i] == 'R':
                            # Mark as reserved/session-active without overwriting ownership
                            spots[i]["state"] = 'R'
                        elif parts[i] == 'O':
                            # Only open if not reserved by a user
                            if spots[i]["state"] != 'R' or not spots[i]["reserved_by"]:
                                spots[i]["state"] = 'O'
                elif line.startswith("PAID:"):
                    try:
                        idx = int(line.split(":", 1)[1]) - 1
                        if 0 <= idx < len(spots):
                            # Clear reservation server-side
                            spots[idx]["reserved_by"] = None
                            spots[idx]["reserved_at"] = None
                            spots[idx]["arrival_ts"] = None
                            spots[idx]["assigned_slot"] = None
                            spots[idx]["arrived"] = None  # Clear arrived flag
                            # Force clear R on system side; keep X if occupied
                            if spots[idx]["state"] != 'X':
                                spots[idx]["state"] = 'O'
                            # Return card to end of server queue (sync with Arduino)
                            push_returned_card_to_queue(idx)
                            # Push updated overlay to device (no R for this spot)
                            push_reservations_to_arduino()
                            # Update capacity holds since reservation is cleared
                            enforce_holds()
                    except Exception:
                        pass
                elif line.startswith("DISP:"):
                    try:
                        # Arduino dispensed a card - sync server queue
                        payload = line.split(":", 1)[1]
                        if payload == "NONE":
                            pass  # No card dispensed
                        else:
                            card_num = int(payload)
                            card_idx = card_num - 1  # Convert 1-based to 0-based
                            if 0 <= card_idx < 4:
                                # Remove this card from server queue (it was dispensed by Arduino)
                                remove_card_from_queue_if_present(card_idx)
                    except Exception:
                        pass
        except Exception:
            time.sleep(0.1)
        time.sleep(0.05)
def enforce_holds():
    """Compute how many slots/cards to hold for imminent reservations; do not mark any slot reserved."""
    try:
        now = time.time()
        hold_count = 0
        for s in spots:
            if s.get("reserved_by"):
                # Don't hold capacity for users who have already arrived
                if s.get("arrived"):
                    continue
                    
                arrival = s.get("arrival_ts")
                if arrival is None:
                    # No arrival time set, hold capacity
                    hold_count += 1
                else:
                    # Check if arrival is within 10 minutes
                    if (arrival - now) <= 10 * 60:
                        hold_count += 1
            else:
                # No reservation, ensure arrival and assignment cleared
                if s.get("arrival_ts"):
                    s["arrival_ts"] = None
                if s.get("assigned_slot"):
                    s["assigned_slot"] = None
                if s.get("arrived"):
                    s["arrived"] = None

        # Push capacity hold (cached to avoid spamming)
        push_capacity_hold_to_arduino(hold_count)
    except Exception:
        pass


def generate_verification_code():
    """Generate a 4-digit verification code"""
    return ''.join(random.choices(string.digits, k=4))

def send_verification_email(email, code):
    """Send verification code to email"""
    try:
        msg = Message(
            'Parking System - Email Verification',
            sender=app.config['MAIL_USERNAME'],
            recipients=[email]
        )
        msg.body = f'Your verification code is: {code}\n\nThis code will expire in 10 minutes.'
        mail.send(msg)
        return True
    except Exception as e:
        print(f"Error sending email: {e}")
        return False

# CSV Database Helper Functions
def load_customers_from_csv():
    """Load customers from CSV file"""
    customers = {}
    if os.path.exists(CUSTOMERS_CSV):
        try:
            with open(CUSTOMERS_CSV, 'r', newline='', encoding='utf-8') as file:
                reader = csv.DictReader(file)
                for row in reader:
                    customers[row['email']] = {
                        'surname': row['surname'],
                        'name': row['name'],
                        'middle_name': row['middle_name'],
                        'phone': row['phone'],
                        'email': row['email'],
                        'rfid': row['rfid'],
                        'password': row['password'] if row['password'] else None,
                        'created_by': row['created_by'],
                        'created_at': row['created_at']
                    }
        except Exception as e:
            print(f"Error loading customers from CSV: {e}")
    return customers

def save_customers_to_csv():
    """Save customers to CSV file"""
    try:
        with open(CUSTOMERS_CSV, 'w', newline='', encoding='utf-8') as file:
            fieldnames = ['email', 'surname', 'name', 'middle_name', 'phone', 'rfid', 'password', 'created_by', 'created_at']
            writer = csv.DictWriter(file, fieldnames=fieldnames)
            writer.writeheader()
            for email, customer in CUSTOMERS.items():
                writer.writerow({
                    'email': email,
                    'surname': customer['surname'],
                    'name': customer['name'],
                    'middle_name': customer['middle_name'],
                    'phone': customer['phone'],
                    'rfid': customer['rfid'],
                    'password': customer['password'] or '',
                    'created_by': customer['created_by'],
                    'created_at': customer['created_at']
                })
    except Exception as e:
        print(f"Error saving customers to CSV: {e}")

def load_verification_codes_from_csv():
    """Load verification codes from CSV file"""
    codes = {}
    if os.path.exists(VERIFICATION_CSV):
        try:
            with open(VERIFICATION_CSV, 'r', newline='', encoding='utf-8') as file:
                reader = csv.DictReader(file)
                for row in reader:
                    # Only load non-expired codes
                    if float(row['timestamp']) > time.time() - 600:  # 10 minutes
                        codes[row['email']] = {
                            'code': row['code'],
                            'timestamp': float(row['timestamp'])
                        }
        except Exception as e:
            print(f"Error loading verification codes from CSV: {e}")
    return codes

def save_verification_codes_to_csv():
    """Save verification codes to CSV file"""
    try:
        with open(VERIFICATION_CSV, 'w', newline='', encoding='utf-8') as file:
            fieldnames = ['email', 'code', 'timestamp']
            writer = csv.DictWriter(file, fieldnames=fieldnames)
            writer.writeheader()
            for email, data in VERIFICATION_CODES.items():
                writer.writerow({
                    'email': email,
                    'code': data['code'],
                    'timestamp': data['timestamp']
                })
    except Exception as e:
        print(f"Error saving verification codes to CSV: {e}")

def load_pending_accounts_from_csv():
    """Load pending accounts from CSV file"""
    accounts = {}
    if os.path.exists(PENDING_CSV):
        try:
            with open(PENDING_CSV, 'r', newline='', encoding='utf-8') as file:
                reader = csv.DictReader(file)
                for row in reader:
                    # Only load non-expired accounts
                    if float(row['timestamp']) > time.time() - 1800:  # 30 minutes
                        accounts[row['email']] = float(row['timestamp'])
        except Exception as e:
            print(f"Error loading pending accounts from CSV: {e}")
    return accounts

def save_pending_accounts_to_csv():
    """Save pending accounts to CSV file"""
    try:
        with open(PENDING_CSV, 'w', newline='', encoding='utf-8') as file:
            fieldnames = ['email', 'timestamp']
            writer = csv.DictWriter(file, fieldnames=fieldnames)
            writer.writeheader()
            for email, timestamp in PENDING_ACCOUNTS.items():
                writer.writerow({
                    'email': email,
                    'timestamp': timestamp
                })
    except Exception as e:
        print(f"Error saving pending accounts to CSV: {e}")

# Load data from CSV files after function definitions
CUSTOMERS = load_customers_from_csv()
VERIFICATION_CODES = load_verification_codes_from_csv()
PENDING_ACCOUNTS = load_pending_accounts_from_csv()

def admin_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not session.get("user") or session.get("user") != "admin":
            return redirect(url_for("login"))
        return view(*args, **kwargs)
    return wrapped

def login_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not session.get("user"):
            return redirect(url_for("login", next=request.path))
        return view(*args, **kwargs)
    return wrapped


@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        
        # Check admin login
        if username in USERS and USERS[username] == password:
            session["user"] = username
            if username == "admin":
                return redirect(url_for("admin_dashboard"))
            return redirect(url_for("index"))
        
        # Check customer login
        if username in CUSTOMERS and CUSTOMERS[username]["password"] == password:
            session["user"] = username
            return redirect(url_for("index"))
            
        return render_template("login.html", error="Invalid credentials")
    return render_template("login.html")


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.route("/admin")
@admin_required
def admin_dashboard():
    return render_template("admin_dashboard.html", customers=CUSTOMERS)

@app.route("/admin/create_customer", methods=["POST"])
@admin_required
def create_customer():
    data = request.get_json()
    surname = data.get("surname", "").strip()
    name = data.get("name", "").strip()
    middle_name = data.get("middle_name", "").strip()
    phone = data.get("phone", "").strip()
    email = data.get("email", "").strip()
    rfid = data.get("rfid", "").strip()
    
    if not all([surname, name, phone, email, rfid]):
        return jsonify({"success": False, "message": "All fields are required"})
    
    # Create username from email
    username = email
    
    if username in CUSTOMERS:
        return jsonify({"success": False, "message": "Customer already exists"})
    
    # Store customer data without password
    CUSTOMERS[username] = {
        "surname": surname,
        "name": name,
        "middle_name": middle_name,
        "phone": phone,
        "email": email,
        "rfid": rfid,
        "password": None,  # No password yet
        "created_by": session.get("user"),
        "created_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    }
    
    # Save to CSV
    save_customers_to_csv()
    
    return jsonify({"success": True, "message": "Customer account created successfully"})

@app.route("/create_password", methods=["GET", "POST"])
def create_password():
    if request.method == "POST":
        email = request.form.get("email", "").strip()
        if not email:
            return render_template("create_password.html", error="Email is required")
        
        if email not in CUSTOMERS:
            return render_template("create_password.html", error="Account not found. Please contact admin.")
        
        if CUSTOMERS[email]["password"] is not None:
            return render_template("create_password.html", error="Password already set for this account")
        
        # Check if verification code already exists and is still valid
        if email in VERIFICATION_CODES:
            stored_data = VERIFICATION_CODES[email]
            # If code is still valid (less than 10 minutes old), don't send new email
            if time.time() - stored_data["timestamp"] < 600:  # 10 minutes
                return render_template("verify_email.html", email=email, message="Verification code already sent. Please check your email.")
        
        # Generate and send verification code only if no valid code exists
        code = generate_verification_code()
        VERIFICATION_CODES[email] = {
            "code": code,
            "timestamp": time.time()
        }
        
        # Save verification code to CSV
        save_verification_codes_to_csv()
        
        if send_verification_email(email, code):
            return render_template("verify_email.html", email=email)
        else:
            return render_template("create_password.html", error="Failed to send verification email")
    
    return render_template("create_password.html")

@app.route("/verify_email", methods=["GET", "POST"])
def verify_email():
    if request.method == "GET":
        # Handle GET request - redirect to create_password
        return redirect(url_for("create_password"))
    
    email = request.form.get("email", "").strip()
    code = request.form.get("code", "").strip()
    
    if email not in VERIFICATION_CODES:
        return render_template("verify_email.html", email=email, error="Invalid email or no verification code found")
    
    stored_data = VERIFICATION_CODES[email]
    
    # Check if code is expired (10 minutes)
    if time.time() - stored_data["timestamp"] > 600:
        del VERIFICATION_CODES[email]
        save_verification_codes_to_csv()
        return render_template("verify_email.html", email=email, error="Verification code expired")
    
    if code != stored_data["code"]:
        return render_template("verify_email.html", email=email, error="Invalid verification code")
    
    # Code is valid, proceed to password creation
    del VERIFICATION_CODES[email]
    PENDING_ACCOUNTS[email] = time.time()
    
    # Save updated data to CSV
    save_verification_codes_to_csv()
    save_pending_accounts_to_csv()
    
    return render_template("set_password.html", email=email)

@app.route("/set_password", methods=["GET", "POST"])
def set_password():
    if request.method == "GET":
        # Handle GET request - redirect to create_password
        return redirect(url_for("create_password"))
    
    email = request.form.get("email", "").strip()
    password = request.form.get("password", "")
    confirm = request.form.get("confirm", "")
    
    # Check if customer exists and doesn't have a password yet
    if email not in CUSTOMERS:
        return render_template("create_password.html", error="Account not found. Please contact admin.")
    
    if CUSTOMERS[email]["password"] is not None:
        return render_template("create_password.html", error="Password already set for this account.")
    
    # Check if session is still valid (if in pending accounts)
    if email in PENDING_ACCOUNTS:
        if time.time() - PENDING_ACCOUNTS[email] > 1800:  # 30 minutes
            del PENDING_ACCOUNTS[email]
            save_pending_accounts_to_csv()
            return render_template("create_password.html", error="Session expired. Please start over.")
    
    if not password or password != confirm:
        return render_template("set_password.html", email=email, error="Passwords do not match")
    
    if len(password) < 6:
        return render_template("set_password.html", email=email, error="Password must be at least 6 characters long")
    
    # Set password for customer
    CUSTOMERS[email]["password"] = password
    
    # Remove from pending accounts if it exists
    if email in PENDING_ACCOUNTS:
        del PENDING_ACCOUNTS[email]
    
    # Save updated data to CSV
    save_customers_to_csv()
    save_pending_accounts_to_csv()
    
    return render_template("password_success.html")

@app.route("/resend_verification", methods=["POST"])
def resend_verification():
    email = request.form.get("email", "").strip()
    
    if not email or email not in CUSTOMERS:
        return render_template("verify_email.html", email=email, error="Invalid email address")
    
    if CUSTOMERS[email]["password"] is not None:
        return render_template("verify_email.html", email=email, error="Password already set for this account")
    
    # Generate new verification code
    code = generate_verification_code()
    VERIFICATION_CODES[email] = {
        "code": code,
        "timestamp": time.time()
    }
    
    # Save verification code to CSV
    save_verification_codes_to_csv()
    
    if send_verification_email(email, code):
        return render_template("verify_email.html", email=email, message="New verification code sent successfully!")
    else:
        return render_template("verify_email.html", email=email, error="Failed to send verification email")


# Static assets (serve images from templates folder for simplicity)
@app.route("/assets/<path:filename>")
def assets(filename):
    return send_from_directory("templates", filename)


@app.route("/")
@login_required
def index():
    return render_template("index.html")


@app.route("/api/spots")
@login_required
def api_spots():
    current_user = session.get("user")
    return jsonify({
        "spots": [
            {
                "id": s["id"],
                "state": s["state"],
                "reserved_by_me": (s["reserved_by"] == current_user),
                "reserved_at": s["reserved_at"],
                "arrival_ts": s.get("arrival_ts")
            } for s in spots
        ]
    })


@app.route("/api/reserve", methods=["POST"])
@login_required
def api_reserve():
    # Auto-assign next card in queue (same as Arduino round-robin behavior)
    data = request.get_json(force=True)
    arrival_ts = float(data.get("arrival_ts", 0))
    now = time.time()
    current_user = session.get("user")

    # If user already holds a reservation, return success idempotently
    mine = next((s for s in spots if s.get("reserved_by") == current_user), None)
    if mine:
        return jsonify({"success": True})

    # Get next card from sequential queue (mirrors Arduino behavior)
    next_card = pop_next_card_from_queue()
    if next_card == -1:
        return jsonify({"success": False, "message": "No available cards"}), 400

    # Assign the next card in queue to the user
    spot = spots[next_card]  # next_card is 0-based, spots are 1-based
    spot["reserved_by"] = current_user
    spot["reserved_at"] = now
    spot["arrival_ts"] = arrival_ts if arrival_ts > 0 else None
    spot["assigned_slot"] = next_card + 1  # Store the assigned card number (1-based)

    # Send commands with delays
    push_reservations_to_arduino()
    time.sleep(0.2)  # Wait 200ms between commands
    push_rfid_data_to_arduino()
    time.sleep(0.1)
    
    # Enforce holds will be called in background read loop
    
    return jsonify({"success": True})


@app.route("/api/cancel", methods=["POST"])
@login_required
def api_cancel():
    # Cancelling reservations is not allowed once reserved
    return jsonify({"success": False, "message": "Cancellation is not allowed after reserving."}), 400


def push_reservations_to_arduino():
    global arduino_serial, spots, last_resv_line
    if not (arduino_serial and arduino_serial.is_open):
        print("Arduino not connected - cannot send RESV command")
        return
    
    # Compose RESV overlay
    codes = []
    for s in spots:
        codes.append('O')
    line = "RESV:" + ",".join(codes) + "\n"
    
    if line != last_resv_line:
        print(f"Sending RESV command: {line.strip()}")
        if send_command_to_arduino(line, "RESV command"):
            last_resv_line = line


def push_capacity_hold_to_arduino(hold_count: int):
    global arduino_serial, last_cap_sent
    if not (arduino_serial and arduino_serial.is_open):
        return
    
    try:
        hold_count = max(0, min(4, int(hold_count)))
    except Exception:
        hold_count = 0
    
    line = f"CAP:{hold_count}\n"
    if line != last_cap_sent:
        print(f"Sending CAP command: {line.strip()}")
        if send_command_to_arduino(line, "CAP command"):
            last_cap_sent = line

def push_rfid_data_to_arduino():
    """Send RFID data for reserved users to Arduino in VIP format"""
    global arduino_serial
    if not (arduino_serial and arduino_serial.is_open):
        return
    
    # Get all RFID codes from customers with reservations
    rfid_pairs = []
    for email, customer in CUSTOMERS.items():
        if customer.get("rfid") and customer.get("password"):
            # Check if this customer has a reservation
            for spot in spots:
                if spot.get("reserved_by") == email:
                    # Format: rfid:email (remove any spaces from RFID)
                    rfid_clean = customer['rfid'].replace(" ", "")
                    rfid_pairs.append(f"{rfid_clean}:{email}")
                    break
    
    if rfid_pairs:
        rfid_data = ",".join(rfid_pairs)
        line = f"RFID:{rfid_data}\n"
        print(f"Sending VIP RFID data: {line.strip()}")
        send_command_to_arduino(line, "VIP RFID data")
    else:
        # Send empty RFID list to clear Arduino
        line = "RFID:\n"
        print("Sending empty VIP RFID data to clear Arduino")
        send_command_to_arduino(line, "Empty VIP RFID data")


def start_timer_on_arduino(spot_id: int):
    global arduino_serial
    if not (arduino_serial and arduino_serial.is_open):
        return
    cmd = f"START:{spot_id}\n"
    try:
        arduino_serial.write(cmd.encode())
    except Exception:
        pass


@app.route("/api/rfid_scan", methods=["POST"])
def api_rfid_scan():
    """Handle RFID card scanning for gate opening and card dispensing"""
    global arduino_serial, spots
    data = request.get_json(force=True)
    rfid_code = data.get("rfid", "").strip().replace(" ", "")  # Remove spaces
    
    if not rfid_code:
        return jsonify({"success": False, "message": "RFID code is required"}), 400
    
    # Find customer by RFID code (compare without spaces)
    customer = None
    for email, customer_data in CUSTOMERS.items():
        customer_rfid = customer_data.get("rfid", "").replace(" ", "")
        if customer_rfid == rfid_code:
            customer = customer_data
            customer["email"] = email
            break
    
    if not customer:
        return jsonify({"success": False, "message": "RFID card not recognized"}), 404
    
    # Check if customer has a password set
    if not customer.get("password"):
        return jsonify({"success": False, "message": "Account not activated. Please complete account setup first."}), 403
    
    # Find the customer's reserved spot
    spot = None
    for s in spots:
        if s.get("reserved_by") == customer["email"]:
            spot = s
            break
    
    if not spot:
        return jsonify({"success": False, "message": "No reservation found for this RFID card"}), 404
    
    if not (arduino_serial and arduino_serial.is_open):
        return jsonify({"success": False, "message": "Arduino not connected"}), 500

    try:
        # Mark user as arrived (they've been assigned a card)
        spot["arrived"] = True
        spot["arrival_ts"] = time.time()  # Mark actual arrival time
        
        # Use HERE command for reserved users
        cmd = f"HERE:{spot['id']}\n"
        print(f"Sending HERE command: {cmd.strip()}")
        
        if send_command_to_arduino(cmd, "HERE command"):
            # Immediately update capacity holds since user has arrived
            time.sleep(0.1)
            enforce_holds()
            
            return jsonify({
                "success": True, 
                "message": f"Welcome {customer['name']}! Gate opening and card dispensing...",
                "customer_name": f"{customer['name']} {customer['surname']}"
            })
        else:
            return jsonify({"success": False, "message": "Failed to send command"}), 500
            
    except Exception as e:
        print(f"Error sending HERE command: {e}")
        return jsonify({"success": False, "message": str(e)}), 500

@app.route("/api/imhere", methods=["POST"])
@login_required
def api_im_here():
    """Legacy I'm here endpoint - kept for backward compatibility"""
    global arduino_serial, spots
    data = request.get_json(force=True)
    spot_id = int(data.get("id", 0)) if data.get("id") is not None else 0
    current_user = session.get("user")

    # If no id provided, infer the user's reserved spot
    spot = None
    if spot_id:
        spot = next((s for s in spots if s["id"] == spot_id), None)
    else:
        # Find the user's own reserved spot
        spot = next((s for s in spots if s.get("reserved_by") == current_user), None)
    if not spot:
        return jsonify({"success": False, "message": "No reserved spot found"}), 404
    if spot.get("reserved_by") != current_user:
        return jsonify({"success": False, "message": "Not your reservation"}), 403

    if not (arduino_serial and arduino_serial.is_open):
        return jsonify({"success": False, "message": "Arduino not connected"}), 500

    try:
        # Mark user as arrived (they've been assigned a card)
        spot["arrived"] = True
        spot["arrival_ts"] = time.time()  # Mark actual arrival time
        
        # Use HERE command for reserved users
        cmd = f"HERE:{spot['id']}\n"
        print(f"Sending HERE command: {cmd.strip()}")
        
        if send_command_to_arduino(cmd, "HERE command"):
            # Immediately update capacity holds since user has arrived
            time.sleep(0.1)
            enforce_holds()
            return jsonify({"success": True})
        else:
            return jsonify({"success": False, "message": "Failed to send command"}), 500
            
    except Exception as e:
        print(f"Error sending HERE command: {e}")
        return jsonify({"success": False, "message": str(e)}), 500


if __name__ == "__main__":
    # Connect to Arduino in the serving process (no reloader)
    ok, msg = connect_arduino()
    print(f"Arduino: {msg}")
    app.run(debug=True, host="0.0.0.0", port=5000, use_reloader=False)


