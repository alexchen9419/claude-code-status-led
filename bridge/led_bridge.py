#!/usr/bin/env python3
"""
Claude Code 狀態燈 bridge：常駐並持有 COM port，
在 127.0.0.1:8765 提供跟 WiFi 版一樣的 HTTP 路徑，轉成序列埠指令。

  pip install pyserial
  python led_bridge.py              # 自動找 Espressif 裝置
  python led_bridge.py --port COM5  # 指定 COM port
"""
import argparse
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import serial
import serial.tools.list_ports

ESPRESSIF_VID = 0x303A           # ESP32-S3 原生 USB
BRIDGE_VIDS = {0x10C4, 0x1A86}   # CP210x / CH34x USB-UART 晶片

lock = threading.Lock()
ser = None
args = None


def find_port():
    if args.port:
        return args.port
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid == ESPRESSIF_VID:
            return p.device
    for p in ports:
        if p.vid in BRIDGE_VIDS:
            return p.device
    return None


def open_serial():
    global ser
    port = find_port()
    if not port:
        return False
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.3
    s.write_timeout = 0.5
    # 開 port 前先把 DTR/RTS 拉低，避免觸發 ESP32 自動重置
    s.dtr = False
    s.rts = False
    s.open()
    ser = s
    print(f"[bridge] 已連線 {port}")
    return True


def send(line: str) -> str:
    """送一行指令並讀回一行回應；失敗時自動重連一次。"""
    global ser
    with lock:
        for attempt in range(2):
            try:
                if ser is None or not ser.is_open:
                    if not open_serial():
                        return "no-device"
                ser.reset_input_buffer()
                ser.write((line + "\n").encode())
                return ser.readline().decode(errors="ignore").strip() or "sent"
            except (serial.SerialException, OSError) as e:
                print(f"[bridge] 序列埠錯誤：{e}，重新連線")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(0.2)
        return "error"


def path_to_cmd(path: str, qs: dict):
    one = lambda k, d: qs.get(k, [d])[0]
    simple = {
        "/idle": "idle", "/working": "working", "/approval": "approval",
        "/done": "done", "/error": "error", "/compact": "compact",
        "/ack": "ack", "/status": "status", "/ping": "ping",
        "/subagent/start": "sub+", "/subagent/stop": "sub-",
    }
    if path in simple:
        return simple[path]
    if path == "/latch":
        return f"latch {one('id', '')}"
    if path == "/flash":
        return f"flash {one('c', 'FFFFFF')} {one('ms', '150')} {one('n', '1')}"
    return None


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        u = urlparse(self.path)
        cmd = path_to_cmd(u.path, parse_qs(u.query))
        if cmd is None:
            self.send_response(404)
            self.end_headers()
            return
        reply = send(cmd)
        body = reply.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_POST = do_GET

    def log_message(self, fmt, *a):
        if args.verbose:
            super().log_message(fmt, *a)


def heartbeat():
    # 每 5 秒 ping 一次，讓 ESP32 知道 bridge 還活著
    while True:
        send("ping")
        time.sleep(5)


def main():
    global args
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="COM port，例如 COM5 或 /dev/ttyACM0")
    ap.add_argument("--listen", default="127.0.0.1")
    ap.add_argument("--http-port", type=int, default=8765)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not open_serial():
        print("[bridge] 找不到裝置，之後收到指令時會再嘗試連線")
    threading.Thread(target=heartbeat, daemon=True).start()

    srv = ThreadingHTTPServer((args.listen, args.http_port), Handler)
    print(f"[bridge] HTTP 監聽 http://{args.listen}:{args.http_port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
