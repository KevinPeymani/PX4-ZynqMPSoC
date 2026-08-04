import time, collections
from pymavlink import mavutil
m = mavutil.mavlink_connection("udpout:127.0.0.1:14550", source_system=255)
m.mav.heartbeat_send(6,8,0,0,0)
loads, counts = [], collections.Counter()
t0=time.time(); last=0.0
while time.time()-t0 < 25.0:
    if time.time()-last > 1.0:
        m.mav.heartbeat_send(6,8,0,0,0); last=time.time()
    msg=m.recv_match(blocking=True, timeout=1.0)
    if msg is None: continue
    counts[msg.get_type()]+=1
    if msg.get_type()=="SYS_STATUS": loads.append(msg.load)
el=time.time()-t0
print("window %.1fs msgs %d" % (el, sum(counts.values())))
print("CPU LOAD n=%d avg=%.1f%%" % (len(loads), sum(loads)/len(loads)/10.0) if loads else "CPU LOAD: none")
