import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

s = serial.Serial(port, 115200, timeout=0.2)
s.setDTR(False)
s.setRTS(False)
time.sleep(0.1)
s.reset_input_buffer()

buf = bytearray()
t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(4096)
    if d:
        buf += d
s.close()

out = buf.decode("utf-8", "replace")
print(out)
print("---- bytes=%d ----" % len(buf))
