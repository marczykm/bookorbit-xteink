import serial,time,sys
s=serial.Serial(sys.argv[1],115200,timeout=1)
s.dtr=False; s.rts=True; time.sleep(.1); s.rts=False
t=time.time(); sent=False; out=open(sys.argv[3],'w')
while time.time()-t<float(sys.argv[2]):
    l=s.readline()
    if l: out.write(l.decode(errors='replace')); out.flush()
    if not sent and time.time()-t>float(sys.argv[2])-12: s.write(b'F'); sent=True
