import sys, time
from pymavlink import mavutil
from pymavlink.dialects.v20 import common as mav
DEV = mav.SERIAL_CONTROL_DEV_SHELL
FLAGS = (mav.SERIAL_CONTROL_FLAG_EXCLUSIVE | mav.SERIAL_CONTROL_FLAG_RESPOND | mav.SERIAL_CONTROL_FLAG_MULTI)
cmds = sys.argv[1].split(";;"); wait = float(sys.argv[2]) if len(sys.argv)>2 else 12.0
m = mavutil.mavlink_connection("udpout:127.0.0.1:14550", source_system=255)
def hb(): m.mav.heartbeat_send(6,8,0,0,0)
hb(); t0=time.time()
while time.time()-t0 < 10:
    if m.recv_match(blocking=True, timeout=1.0) is not None: break
    hb()
def send(s):
    b=s.encode()
    while b:
        c,b=b[:70],b[70:]
        m.mav.serial_control_send(DEV,FLAGS,0,0,len(c),c+b"\x00"*(70-len(c))); time.sleep(0.06)
out=bytearray()
for c in cmds: send(c+"\n"); time.sleep(0.4)
t0=time.time(); last=0.0
while time.time()-t0 < wait:
    if time.time()-last>1.0: hb(); last=time.time()
    msg=m.recv_match(type="SERIAL_CONTROL", blocking=True, timeout=0.5)
    if msg and msg.device==DEV and msg.count: out+=bytes(msg.data[:msg.count])
sys.stdout.write(out.decode("utf8","replace"))
