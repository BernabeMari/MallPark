from flask import Flask, render_template, request, jsonify, redirect, url_for, session, send_from_directory
from functools import wraps
import serial
import serial.tools.list_ports
import threading
import time

app = Flask(__name__)
app.secret_key = "change-this-secret-key"

# In-memory users (demo only)
USERS = {
    "customer": "password"
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
        arduino_serial = serial.Serial(port, baudrate, timeout=1)
        time.sleep(2)
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
        if username in USERS and USERS[username] == password:
            session["user"] = username
            return redirect(url_for("index"))
        return render_template("login.html", error="Invalid credentials")
    return render_template("login.html")


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.route("/register", methods=["GET", "POST"])
def register():
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        confirm = request.form.get("confirm", "")
        if not username or not password:
            return render_template("register.html", error="All fields are required")
        if password != confirm:
            return render_template("register.html", error="Passwords do not match")
        if username in USERS:
            return render_template("register.html", error="Username already exists")
        USERS[username] = password
        session["user"] = username
        return redirect(url_for("index"))
    return render_template("register.html")


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

    # Do not change visual state; capacity hold will be sent to MCU
    push_reservations_to_arduino()
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
    # Compose RESV overlay but do not mark 'R' from server-side holds
    codes = []
    for s in spots:
        codes.append('O')
    line = "RESV:" + ",".join(codes) + "\n"
    if line != last_resv_line:
        print(f"Sending RESV command: {line.strip()}")
        try:
            arduino_serial.write(line.encode())
            print("RESV command sent successfully")
            last_resv_line = line
        except Exception as e:
            print(f"Error sending RESV command: {e}")


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
        try:
            arduino_serial.write(line.encode())
            last_cap_sent = line
        except Exception as e:
            print(f"Error sending CAP command: {e}")


def start_timer_on_arduino(spot_id: int):
    global arduino_serial
    if not (arduino_serial and arduino_serial.is_open):
        return
    cmd = f"START:{spot_id}\n"
    try:
        arduino_serial.write(cmd.encode())
    except Exception:
        pass


@app.route("/api/imhere", methods=["POST"])
@login_required
def api_im_here():
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
    # Allow HERE even if not visually reserved; MCU will handle held capacity

    if not (arduino_serial and arduino_serial.is_open):
        return jsonify({"success": False, "message": "Arduino not connected"}), 500

    try:
        # Mark user as arrived (they've been assigned a card)
        spot["arrived"] = True
        spot["arrival_ts"] = time.time()  # Mark actual arrival time
        
        # Use HERE command for reserved users
        cmd = f"HERE:{spot['id']}\n"
        print(f"Sending HERE command: {cmd.strip()}")
        arduino_serial.write(cmd.encode())
        print("HERE command sent successfully")
        
        # Immediately update capacity holds since user has arrived
        enforce_holds()
        
        return jsonify({"success": True})
    except Exception as e:
        print(f"Error sending HERE command: {e}")
        return jsonify({"success": False, "message": str(e)}), 500


if __name__ == "__main__":
    # Connect to Arduino in the serving process (no reloader)
    ok, msg = connect_arduino()
    print(f"Arduino: {msg}")
    app.run(debug=True, host="0.0.0.0", port=5000, use_reloader=False)


