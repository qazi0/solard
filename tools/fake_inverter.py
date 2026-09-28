#!/usr/bin/env python3
"""A fake GoodWe hybrid inverter (Modbus/TCP, read-only) for testing solard.

    python3 tools/fake_inverter.py 15020          # listen on 127.0.0.1:15020
    solard --host 127.0.0.1 --port 15020 --http 18768

Serves synthetic values: 1 kW solar, 900 W house, battery discharging 520 W at 60 %.
Only function 0x03 (read holding registers) is answered.
"""
import socketserver, struct, sys

def registers():
    r = [0] * 125                       # 35100..35224
    def u16(a, v): r[a - 35100] = int(v) & 0xffff
    def u32(a, v): v = int(v) & 0xffffffff; r[a - 35100] = v >> 16; r[a - 35099] = v & 0xffff
    u16(35103, 3000); u16(35104, 20); u32(35105, 600)          # PV1 300 V 2 A 600 W
    u16(35107, 2800); u16(35108, 14); u32(35109, 400)          # PV2 280 V 1.4 A 400 W
    u16(35120, 0x0202); u16(35136, 1)                         # both strings producing, grid connected
    u16(35138, 900); u16(35140, 0); u16(35145, 2300); u16(35146, 39); u16(35147, 5000); u16(35150, 900)
    u16(35170, 900); u16(35174, 380); u16(35176, 420); u16(35178, 4000)
    u16(35180, 520); u16(35181, 100); u32(35182, 520); u16(35184, 2); u16(35187, 1)
    u32(35193, 50); u16(35205, 45); u16(35211, 26)             # today's counters (0.1 kWh)
    b = [0] * 24                        # 37000..37023 (BMS)
    b[3] = 250; b[4] = 100; b[5] = 100; b[7] = 60; b[8] = 100; b[9] = 1
    return {35100: r, 37000: b, 45356: [10, 0, 10], 47760: [95]}

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        while True:
            q = b''
            while len(q) < 12:
                x = s.recv(12 - len(q))
                if not x: return
                q += x
            tid, _, _, unit, fn, addr, cnt = struct.unpack('>HHHBBHH', q)
            data = None
            for base, arr in registers().items():
                if fn == 3 and base <= addr and addr + cnt <= base + len(arr):
                    data = arr[addr - base:addr - base + cnt]
            if data is None:                                   # illegal address / function
                s.sendall(struct.pack('>HHHBBB', tid, 0, 3, unit, fn | 0x80, 2)); continue
            body = struct.pack('>BB', 3, cnt * 2) + struct.pack('>%dH' % cnt, *data)
            s.sendall(struct.pack('>HHHB', tid, 0, len(body) + 1, unit) + body)

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

if __name__ == '__main__':
    Server(('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 15020), Handler).serve_forever()
