#!/usr/bin/env python3
"""Watch Dot Desktop's WebSocket stream like the browser page does, and save what it shows.

    python3 tools/ws_probe.py HOST[:PORT] out.png [seconds] [--click X Y] [--type TEXT]

Reassembles the JPEG tiles onto a canvas (needs Pillow), prints fps and bytes, and can send a
click and some keys first. No other dependencies: the WebSocket client is the minimum here.
"""
import base64, io, os, socket, struct, sys, time

from PIL import Image


def connect(host, port):
    s = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (host, port, key)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(1024)
    head, rest = buf.split(b"\r\n\r\n", 1)
    if b" 101 " not in head.split(b"\r\n")[0]:
        raise SystemExit("no upgrade: %r" % head[:80])
    return s, rest


def send_text(s, text):
    data = text.encode()
    mask = os.urandom(4)
    hdr = bytes([0x81, 0x80 | len(data)]) + mask
    s.sendall(hdr + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


class Reader:
    def __init__(self, s, rest):
        self.s, self.buf = s, rest

    def need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def frame(self):
        b0, b1 = self.need(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self.need(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self.need(8))[0]
        return b0 & 0x80, b0 & 0x0F, self.need(n)

    def message(self):
        fin, op, data = self.frame()
        while not fin:
            fin, _, more = self.frame()
            data += more
        return op, data


def main():
    hp, out = sys.argv[1], sys.argv[2]
    secs = float(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 8
    host, port = (hp.split(":") + ["80"])[:2]
    s, rest = connect(host, int(port))
    r = Reader(s, rest)
    canvas = None
    tiles = frames = total = 0
    args = sys.argv
    t0 = time.time()
    sent = False
    while time.time() - t0 < secs:
        s.settimeout(max(0.1, secs - (time.time() - t0)))
        try:
            op, data = r.message()
        except (socket.timeout, EOFError):
            break
        total += len(data)
        if op == 1:
            import json
            m = json.loads(data)
            print("text", m)
            if "size" in m:
                canvas = Image.new("RGB", tuple(m["size"]))
        elif op == 2 and data[:1] == b"T":
            x, y, w, h = struct.unpack("<4H", data[1:9])
            im = Image.open(io.BytesIO(data[9:]))
            im.load()
            if im.size != (w, h):
                print("tile size mismatch", im.size, (w, h))
            if canvas:
                canvas.paste(im.convert("RGB"), (x, y))
            tiles += 1
        elif op == 2 and data[:1] == b"F":
            frames += 1
            if frames == 1 and not sent:
                sent = True
                if "--click" in args:
                    i = args.index("--click")
                    cx, cy = int(args[i + 1]), int(args[i + 2])
                    for b in (0, 1, 0):
                        send_text(s, "m %d %d %d" % (cx, cy, b))
                        time.sleep(0.15)
                if "--type" in args:
                    for ch in args[args.index("--type") + 1]:
                        send_text(s, "k " + ch)
                        time.sleep(0.05)
    dt = time.time() - t0
    print("%d frames, %d tiles, %d bytes in %.1f s (%.1f fps, %.0f kB/s)"
          % (frames, tiles, total, dt, frames / dt, total / 1024 / dt))
    if canvas:
        canvas.save(out)
        print("saved", out)


if __name__ == "__main__":
    main()
