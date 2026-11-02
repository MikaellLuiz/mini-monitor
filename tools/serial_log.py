"""Le o serial por N segundos sem resetar a placa, imprimindo em tempo real.

Uso: serial_log.py [porta] [segundos]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.3
s.dtr = s.rts = False
s.open()
s.dtr = s.rts = False
end, got = time.time() + secs, False
while time.time() < end:
    data = s.read(512)
    if data:
        got = True
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
if not got:
    print("NADA RECEBIDO")
