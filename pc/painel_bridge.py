"""
Painel bridge - one program that feeds the ESP32 dashboard (and the small OLED monitor).

Replaces bridge.pyw + clawdmeter.py + codexmeter.py. It sends:
  - PC hardware stats (LibreHardwareMonitor web server) once per second
  - Claude and Codex usage every few seconds

over two paths at the same time, so it works whichever way the board is reachable:
  - USB: every USB serial port that is plugged in
  - Wi-Fi: UDP on the local network (broadcast + directly to the ESP32 if it can be found)

Run it with run_painel.bat to see what it is doing.
Needs: pip install requests pyserial
"""
import datetime
import json
import os
import socket
import threading
import time

import requests

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None

# ---------------- config ----------------
SERIAL_PORTS = []            # [] = every USB serial device; or e.g. ["COM5", "COM7"]
BAUD = 115200

UDP_PORT = 4210              # must match PC_UDP_PORT in the sketch
ESP_HOST = "painel.local"    # or the IP shown on the dashboard's "System" page, e.g. "192.168.1.80"

LHM_URL = "http://localhost:8085/data.json"

USAGE_POLL_S = 120           # how often Claude / Codex are asked
USAGE_SEND_S = 5             # how often the last known usage is re-sent
USAGE_MAX_AGE_S = 900        # stop sending usage older than this

CLAUDE_CREDENTIALS = os.path.expanduser("~/.claude/.credentials.json")
CODEX_AUTH = os.path.expanduser("~/.codex/auth.json")


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


# ---------------- USB serial ----------------
ports = {}         # port name -> open serial.Serial
port_errors = {}   # port name -> last reported problem (so each is logged once)

def refresh_ports():
    """Open any wanted port that is not open yet (boards can be plugged in later)."""
    if serial is None:
        return
    try:
        wanted = SERIAL_PORTS or [p.device for p in list_ports.comports() if p.vid is not None]
    except Exception:
        return
    for name in wanted:
        if name in ports:
            continue
        try:
            ser = serial.Serial()
            ser.port = name
            ser.baudrate = BAUD
            ser.timeout = 0
            ser.write_timeout = 1
            ser.dtr = False      # keep DTR/RTS low so opening the port does not reset the board
            ser.rts = False
            ser.open()
            ports[name] = ser
            port_errors.pop(name, None)
            log(f"USB: {name} opened")
        except Exception as e:
            text = str(e)
            if "PermissionError" in text or "denied" in text.lower() or "negado" in text.lower():
                reason = "in use by another program (Arduino IDE / Serial Monitor / old bridge?)"
            else:
                reason = text
            if port_errors.get(name) != reason:
                port_errors[name] = reason
                log(f"USB: {name} did not open - {reason}")

def send_serial(data):
    for name, ser in list(ports.items()):
        try:
            ser.write(data)
            ser.reset_input_buffer()   # discard the board's debug output
        except Exception:
            log(f"USB: {name} lost")
            try:
                ser.close()
            except Exception:
                pass
            del ports[name]


# ---------------- Wi-Fi (UDP) ----------------
udp_socks = {}     # local IP -> socket bound to that network interface (for broadcast)
unicast_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
esp_ip = None      # resolved address of the ESP32, if found

def local_ips():
    try:
        ips = socket.gethostbyname_ex(socket.gethostname())[2]
    except OSError:
        return []
    return [ip for ip in ips if not ip.startswith("127.")]

def refresh_udp():
    current = set(local_ips())
    for ip in list(udp_socks):
        if ip not in current:
            udp_socks.pop(ip).close()
    for ip in current - set(udp_socks):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.bind((ip, 0))
            udp_socks[ip] = s
            log(f"Network: broadcasting from {ip}")
        except OSError:
            pass

def send_udp(data):
    for ip, s in list(udp_socks.items()):
        try:
            s.sendto(data, ("255.255.255.255", UDP_PORT))
        except OSError:
            udp_socks.pop(ip).close()
    if esp_ip:
        try:
            unicast_sock.sendto(data, (esp_ip, UDP_PORT))
        except OSError:
            pass

def resolve_loop():
    """Background thread: find the ESP32's address (name lookups can block for seconds)."""
    global esp_ip
    while True:
        try:
            ip = socket.gethostbyname(ESP_HOST)
        except OSError:
            ip = None
        if ip != esp_ip:
            esp_ip = ip
            log(f"Network: ESP32 found at {ip}" if ip else f"Network: {ESP_HOST} not found (broadcast only)")
        time.sleep(30)


def send(line):
    data = line.encode("utf-8")
    send_serial(data)
    send_udp(data)


# ---------------- LibreHardwareMonitor ----------------
def get_hw_by_icon(node, icons):
    # LHM uses ImageURLs like "images_icon/cpu.png"
    if any(icon in node.get("ImageURL", "") for icon in icons):
        return node
    for child in node.get("Children", []):
        res = get_hw_by_icon(child, icons)
        if res:
            return res
    return None

def get_stat(node, type_name, name_contains=None):
    if node is None:
        return None
    if node.get("Type") == type_name:
        if name_contains is None or name_contains.lower() in node.get("Text", "").lower():
            try:
                return float(node.get("Value", "").replace(",", ".").split()[0])
            except (ValueError, IndexError):
                pass
    for child in node.get("Children", []):
        res = get_stat(child, type_name, name_contains)
        if res is not None:
            return res
    return None

def hardware_line():
    """cpu%,cpuTemp,gpu%,gpuTemp,ramGB,ram%"""
    data = requests.get(LHM_URL, timeout=1).json()

    cpu_node = get_hw_by_icon(data, ["cpu.png"])
    gpu_node = get_hw_by_icon(data, ["nvidia.png", "ati.png", "amd.png"])
    ram_node = get_hw_by_icon(data, ["ram.png"])

    cpu_u = get_stat(cpu_node, "Load", "Total") or get_stat(cpu_node, "Load") or 0
    cpu_t = (get_stat(cpu_node, "Temperature", "Package") or get_stat(cpu_node, "Temperature", "Tdie")
             or get_stat(cpu_node, "Temperature") or 0)

    gpu_u = get_stat(gpu_node, "Load", "Core") or get_stat(gpu_node, "Load") or 0
    gpu_t = get_stat(gpu_node, "Temperature", "Core") or get_stat(gpu_node, "Temperature") or 0

    ram_gb = get_stat(ram_node, "Data", "Used") or 0.0
    ram_u = get_stat(ram_node, "Load", "Memory") or get_stat(ram_node, "Load") or 0

    return f"{int(cpu_u)},{int(cpu_t)},{int(gpu_u)},{int(gpu_t)},{ram_gb:.1f},{int(ram_u)}\n"


# ---------------- Claude / Codex usage ----------------
# each fetch returns (session %, session reset epoch or None, weekly %, weekly reset epoch or None)

def iso_to_epoch(s):
    if not s:
        return None
    try:
        return datetime.datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None

def fetch_claude():
    with open(CLAUDE_CREDENTIALS, "r") as f:
        data = json.load(f)
    token = data.get("accessToken") or data.get("token") or (data.get("claudeAiOauth") or {}).get("accessToken")
    if not token:
        raise ValueError("token not found in .credentials.json")

    r = requests.get("https://api.anthropic.com/api/oauth/usage", timeout=15, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
    })
    if r.status_code == 401:
        raise RuntimeError("token expired (open Claude Code to renew it)")
    r.raise_for_status()
    d = r.json()
    five, seven = d.get("five_hour") or {}, d.get("seven_day") or {}
    return (float(five.get("utilization") or 0), iso_to_epoch(five.get("resets_at")),
            float(seven.get("utilization") or 0), iso_to_epoch(seven.get("resets_at")))

def fetch_codex():
    with open(CODEX_AUTH, "r") as f:
        data = json.load(f)
    token = (data.get("accessToken") or data.get("personal_access_token")
             or (data.get("tokens") or {}).get("access_token"))
    if not token:
        raise ValueError("token not found in auth.json")

    r = requests.get("https://chatgpt.com/backend-api/wham/usage", timeout=15, headers={
        "Authorization": f"Bearer {token}",
        "User-Agent": "Mozilla/5.0",
        "Content-Type": "application/json",
    })
    if r.status_code == 401:
        raise RuntimeError("token expired (open Codex to renew it)")
    r.raise_for_status()
    rate = r.json().get("rate_limit") or {}
    primary, secondary = rate.get("primary_window") or {}, rate.get("secondary_window") or {}
    now = time.time()

    def reset(window):
        secs = window.get("reset_after_seconds")
        return None if secs is None else now + secs

    return (float(primary.get("used_percent") or 0), reset(primary),
            float(secondary.get("used_percent") or 0), reset(secondary))

PROVIDERS = {"C": ("Claude", fetch_claude), "X": ("Codex", fetch_codex)}   # line prefix -> (name, fetch)
usage = {}          # prefix -> (values tuple, fetched_at)
usage_errors = {}   # prefix -> last reported problem

def usage_loop():
    """Background thread: the usage APIs can be slow, keep them out of the 1 Hz loop."""
    while True:
        for prefix, (name, fetch) in PROVIDERS.items():
            try:
                usage[prefix] = (fetch(), time.time())
                usage_errors.pop(prefix, None)
                s_pct, _, w_pct, _ = usage[prefix][0]
                log(f"{name}: 5h {s_pct:.0f}%  7d {w_pct:.0f}%")
            except Exception as e:
                if usage_errors.get(prefix) != str(e):
                    usage_errors[prefix] = str(e)
                    log(f"{name}: failed - {e}")
        time.sleep(USAGE_POLL_S)

def usage_lines():
    """C,<5h %>,<5h reset in s>,<7d %>,<7d reset in s>   (X,... = Codex; -1 = unknown)"""
    now = time.time()
    lines = []
    for prefix, ((s_pct, s_reset, w_pct, w_reset), fetched) in list(usage.items()):
        if now - fetched > USAGE_MAX_AGE_S:
            continue
        s_left = -1 if s_reset is None else max(0, int(s_reset - now))
        w_left = -1 if w_reset is None else max(0, int(w_reset - now))
        lines.append(f"{prefix},{s_pct:.0f},{s_left},{w_pct:.0f},{w_left}\n")
    return lines


# ---------------- main ----------------
def main():
    log("Painel bridge starting (Ctrl+C to quit)")
    if serial is None:
        log("USB: pyserial not installed (pip install pyserial) - Wi-Fi only")
    threading.Thread(target=usage_loop, daemon=True).start()
    threading.Thread(target=resolve_loop, daemon=True).start()

    tick = 0
    lhm_ok = None
    try:
        while True:
            if tick % 5 == 0:
                refresh_ports()
                refresh_udp()

            try:
                send(hardware_line())
                if lhm_ok is not True:
                    lhm_ok = True
                    log("PC: sending CPU/GPU/RAM")
            except Exception as e:
                if lhm_ok is not False:
                    lhm_ok = False
                    log(f"PC: LibreHardwareMonitor is not responding at {LHM_URL} - {e}")

            if tick % USAGE_SEND_S == 0:
                for line in usage_lines():
                    send(line)

            if tick % 60 == 59:
                usb = ", ".join(ports) or "none"
                net = ", ".join(udp_socks) or "none"
                log(f"status: USB [{usb}]  network [{net}]  ESP32 [{esp_ip or 'broadcast only'}]")

            tick += 1
            time.sleep(1)
    except KeyboardInterrupt:
        log("Exiting...")
    finally:
        for ser in ports.values():
            try:
                ser.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
