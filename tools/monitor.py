import serial,time,sys
s=serial.Serial(sys.argv[1],115200,timeout=1)
s.dtr=False; s.rts=True; time.sleep(.1); s.rts=False
t=time.time()
while time.time()-t<float(sys.argv[2]):
    l=s.readline()
    if l: print(l.decode(errors='replace').rstrip(),flush=True)
