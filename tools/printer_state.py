import json
import os
import re
import ssl
import paho.mqtt.client as mqtt

# Printer details are read from the sketch's credentials.h (not committed).
HERE = os.path.dirname(os.path.abspath(__file__))
CREDENTIALS = os.path.join(HERE, "..", "Bambu_Monitor_ESP32", "credentials.h")
with open(CREDENTIALS, encoding="utf-8") as f:
    _creds = dict(re.findall(r'const char\*\s+(\w+)\s*=\s*"([^"]*)"', f.read()))

PRINTER_IP = _creds["PRINTER_IP"]
ACCESS_CODE = _creds["ACCESS_CODE"]

MQTT_PORT = 8883
DISCOVERY_TOPIC = "device/+/report"

OUTPUT_FILE = os.path.join(HERE, "bambu_dump.json")

serial_number = None
pushall_sent = False
printer_name = None
report = None


def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        print(f"Connected to {PRINTER_IP}")
        client.subscribe(DISCOVERY_TOPIC)
        print("Waiting for printer response...")
    else:
        print(f"Connection failed: {reason_code}")


def on_message(client, userdata, msg):
    global serial_number, pushall_sent, printer_name, report

    try:
        data = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        return

    parts = msg.topic.split("/")

    if len(parts) < 3:
        return

    detected_serial = parts[1]

    # First message: discover serial and request full state
    if not pushall_sent:
        serial_number = detected_serial

        request_topic = f"device/{serial_number}/request"

        payload = {
            "pushing": {
                "sequence_id": "1",
                "command": "pushall",
                "version": 1,
                "push_target": 1
            }
        }

        client.publish(
            request_topic,
            json.dumps(payload)
        )

        # The printer's name comes from get_version, not the status report
        client.publish(
            request_topic,
            json.dumps({"info": {"sequence_id": "2", "command": "get_version"}})
        )

        pushall_sent = True
        print(f"Detected serial: {serial_number}")
        print("Requested full printer state...")
        return

    info = data.get("info", {})
    if info.get("command") == "get_version":
        for module in info.get("module", []):
            if module.get("name") == "ota":
                printer_name = module.get("product_name")
    elif "print" in data and report is None:
        report = data

    if report is None or printer_name is None:
        return

    # Have both the full report and the name: save and exit
    data = report
    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        json.dump(
            data,
            f,
            indent=2,
            sort_keys=True
        )

    print(f"Saved full printer dump to: {OUTPUT_FILE}")
    print_summary(data, printer_name)

    client.disconnect()


def unpack_temp(raw):
    # Some printers pack temps as (target << 16) | current; keep the current value.
    if raw is not None and raw > 65535:
        return int(raw) & 0xFFFF
    return raw


def print_summary(data, printer_name):
    p = data.get("print", {})

    # -------------------------
    # Printer
    # -------------------------

    state = p.get("gcode_state", "UNKNOWN")

    device = p.get("device", {})

    bed_temp = unpack_temp(
        device
        .get("bed", {})
        .get("info", {})
        .get("temp")
    )

    chamber_temp = unpack_temp(
        device
        .get("ctc", {})
        .get("info", {})
        .get("temp")
    )

    # -------------------------
    # Extruders / nozzles
    # -------------------------

    extruders = (
        device
        .get("extruder", {})
        .get("info", [])
    )

    left_nozzle = None
    right_nozzle = None

    # Extruder 0 is the right nozzle, 1 the left (same as the sketch)
    for extruder in extruders:
        if extruder.get("id") == 0:
            right_nozzle = unpack_temp(extruder.get("temp"))

        elif extruder.get("id") == 1:
            left_nozzle = unpack_temp(extruder.get("temp"))

    # -------------------------
    # AMS
    # -------------------------

    ams_units = (
        p.get("ams", {})
        .get("ams", [])
    )

    ams_info = []

    for ams in ams_units:
        ams_info.append({
            "id": ams.get("id"),
            "temperature": ams.get("temp"),
            "humidity_level": ams.get("humidity"),
            "humidity_raw": ams.get("humidity_raw"),
        })

    # -------------------------
    # Network
    # -------------------------

    wifi = p.get("wifi_signal")

    # -------------------------
    # Print status
    # -------------------------

    progress = p.get("mc_percent")
    remaining_minutes = p.get("mc_remaining_time")

    layer = p.get("layer_num")
    total_layers = p.get("total_layer_num")

    job_name = p.get("subtask_name")

    # -------------------------
    # Output
    # -------------------------

    print()
    print(printer_name or "Bambu printer")
    print("=" * 32)

    print(f"State:        {state}")

    print()
    print("Temperatures")
    print(f"  Chamber:    {chamber_temp} °C")
    print(f"  Bed:        {bed_temp} °C")
    print(f"  Left nozzle:{left_nozzle} °C")
    print(f"  Right nozzle:{right_nozzle} °C")

    for ams in ams_info:
        print()
        print(f"AMS {ams['id']}")
        print(f"  Temp:       {ams['temperature']} °C")
        print(f"  Humidity:   {ams['humidity_level']}")
        print(f"  Raw:        {ams['humidity_raw']}")

    print()
    print(f"Wi-Fi:        {wifi}")

    # Only show print information when relevant
    if state not in ("IDLE", "FINISH"):

        print()
        print("Print")

        if job_name:
            print(f"  Job:        {job_name}")

        if progress is not None:
            print(f"  Progress:   {progress}%")

        if layer is not None:
            print(f"  Layer:      {layer}/{total_layers}")

        if remaining_minutes is not None:
            hours, minutes = divmod(
                int(remaining_minutes), 60
            )

            if hours:
                print(f"  Remaining:  {hours}h {minutes}m")
            else:
                print(f"  Remaining:  {minutes}m")


client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2
)

client.username_pw_set(
    username="bblp",
    password=ACCESS_CODE
)

client.tls_set(
    cert_reqs=ssl.CERT_NONE
)

client.tls_insecure_set(True)

client.on_connect = on_connect
client.on_message = on_message

client.connect(
    PRINTER_IP,
    MQTT_PORT,
    keepalive=60
)

client.loop_forever()