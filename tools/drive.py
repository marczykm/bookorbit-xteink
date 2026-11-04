"""Drives the device over serial and saves screenshots.
usage: drive.py PORT OUTDIR STEP...   steps: wait:SECONDS | a key char (o d u n p b) | shot | CMD:text (a serial command line)
Resets the device first. Screenshots land in OUTDIR/shot-N.png, the log in OUTDIR/log.txt."""
import os, re, subprocess, sys, time, serial
port, outdir, steps = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(outdir, exist_ok=True)
s = serial.Serial(port, 115200, timeout=0.2)
try:
    s.dtr = False; s.rts = True; time.sleep(.1); s.rts = False
except OSError:  # the simulator's pseudo-terminal has no reset line (press R in its window to reset)
    pass
log = open(os.path.join(outdir, "log.txt"), "w")
buf = ""; shots = 0

def pump(seconds):
    global buf
    end = time.time() + seconds
    while time.time() < end:
        d = s.read(4096).decode(errors="replace")
        if d:
            buf += d; log.write(d); log.flush()

def shot():
    global buf, shots
    buf = ""
    s.write(b"F"); 
    t = time.time()
    while "FB-END" not in buf and time.time() - t < 60: pump(0.3)
    shots += 1
    # Log lines the firmware prints while the dump is running (panel refresh notices) land inside the hex: strip them.
    body = buf.split("FB-BEGIN", 1)[-1].split("FB-END", 1)[0]
    body = re.sub(r"\[\d+\][^\n]*\n", "", body)
    buf = "FB-BEGIN\n" + re.sub(r"[^0-9a-f]", "", body) + "\nFB-END\n"
    raw = os.path.join(outdir, f"shot-{shots}.txt"); open(raw, "w").write(buf)
    subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "fb2png.py"), raw, raw.replace(".txt", ".png")])
    os.remove(raw)

for step in steps:
    if step.startswith("wait:"): pump(float(step[5:]))
    elif step == "shot": shot()
    elif step.startswith("CMD:"): s.write((step[4:] + "\n").encode()); pump(0.3)
    elif step.startswith("OPEN:"): s.write((step + "\n").encode()); pump(0.3)
    else: s.write(step.encode()); pump(0.3)
