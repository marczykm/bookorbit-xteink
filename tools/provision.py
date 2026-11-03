"""Sends the settings to the device over USB serial; the device saves them to its SD card as /bookorbit.json.
usage: provision.py PORT SSID WIFI_PASSWORD SERVER USERNAME PASSWORD [insecure]"""
import json, sys, time, serial
port, ssid, wifipw, server, user, pw = sys.argv[1:7]
insecure = len(sys.argv) > 7 and sys.argv[7] == "insecure"
s = serial.Serial(port, 115200, timeout=1)
s.dtr = False; s.rts = True; time.sleep(.1); s.rts = False  # reset into the app
time.sleep(14)  # let it reach the library (or an error screen)
s.write(("CFG:" + json.dumps(dict(ssid=ssid, wifiPassword=wifipw, server=server, username=user, password=pw, insecureTls=insecure)) + "\n").encode())
t = time.time()
while time.time() - t < 8:
    l = s.readline().decode(errors="replace").strip()
    if l: print(l)
