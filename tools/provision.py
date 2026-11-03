"""Sends the settings to the device over USB serial; the device saves them to its SD card as /bookorbit.json.
usage: provision.py PORT SSID WIFI_PASSWORD SERVER USERNAME PASSWORD [insecure|secure [TIMEZONE]]
TIMEZONE is a POSIX TZ string such as CET-1CEST,M3.5.0,M10.5.0/3"""
import json, sys, time, serial
port, ssid, wifipw, server, user, pw = sys.argv[1:7]
insecure = len(sys.argv) > 7 and sys.argv[7] == "insecure"
timezone = sys.argv[8] if len(sys.argv) > 8 else ""
s = serial.Serial(port, 115200, timeout=1)
s.dtr = False; s.rts = True; time.sleep(.1); s.rts = False  # reset into the app
time.sleep(14)  # let it reach the library (or an error screen)
s.write(("CFG:" + json.dumps(dict(ssid=ssid, wifiPassword=wifipw, server=server, username=user, password=pw, insecureTls=insecure, timezone=timezone)) + "\n").encode())
t = time.time()
while time.time() - t < 8:
    l = s.readline().decode(errors="replace").strip()
    if l: print(l)
