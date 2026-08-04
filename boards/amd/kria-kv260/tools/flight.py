import time
from pymavlink import mavutil
from pymavlink.dialects.v20 import common as mav

m = mavutil.mavlink_connection("udpout:127.0.0.1:14550", source_system=255)
_last_hb = [0.0]; _last_mc = [0.0]
LOG = {"chunks": {}, "seen": set(), "first": None, "last": None}

def log_frame(msg):
    # The header arrives as LOGGING_DATA_ACKED and PX4 blocks the whole stream
    # until the GCS acknowledges it (mavlink_ulog.cpp retries ACK_MAX_TRIES then
    # gives up). Without this reply only that single frame ever arrives.
    if msg.get_type() == "LOGGING_DATA_ACKED":
        m.mav.logging_ack_send(1, 1, msg.sequence)
    """LOGGING_DATA is unacked by design, so gaps are expected; key chunks by
    sequence and report continuity rather than assuming the stream is whole."""
    seq = msg.sequence
    if seq in LOG["seen"]:
        return
    LOG["seen"].add(seq)
    LOG["chunks"][seq] = bytes(msg.data[:msg.length])
    if LOG["first"] is None or seq < LOG["first"]: LOG["first"] = seq
    if LOG["last"] is None or seq > LOG["last"]: LOG["last"] = seq

def write_ulg(path):
    if not LOG["chunks"]:
        print("  ULOG: no LOGGING_DATA received"); return
    lo, hi = LOG["first"], LOG["last"]
    span = hi - lo + 1
    missing = [s for s in range(lo, hi+1) if s not in LOG["chunks"]]
    data = b"".join(LOG["chunks"][s] for s in sorted(LOG["chunks"]))
    open(path, "wb").write(data)
    magic_ok = data[:4] == b"ULog"
    print("  ULOG: %s  %d bytes, frames %d/%d, missing %d (%.2f%%), magic=%s"
          % (path, len(data), len(LOG["chunks"]), span, len(missing),
             100.0*len(missing)/span if span else 0, "ULog OK" if magic_ok else data[:4]))

def keepalive():
    """Heartbeat + MANUAL_CONTROL. COM_RC_IN_MODE=1 is joystick-only, so
    without MANUAL_CONTROL the RC_RECEIVER health bit stays bad and commander
    answers Arming denied: Resolve system health failures first."""
    now = time.time()
    if now - _last_hb[0] > 0.5:
        m.mav.heartbeat_send(6, 8, 0, 0, 0); _last_hb[0] = now
    if now - _last_mc[0] > 0.1:
        m.mav.manual_control_send(1, 0, 0, 500, 0, 0); _last_mc[0] = now

def pump(dur, tag=None, show_text=True):
    t0 = time.time(); alt = []; armed = None; mode = None
    while time.time() - t0 < dur:
        keepalive()
        msg = m.recv_match(blocking=True, timeout=0.2)
        if msg is None: continue
        t = msg.get_type()
        if t == "HEARTBEAT":
            armed = bool(msg.base_mode & mav.MAV_MODE_FLAG_SAFETY_ARMED)
            mode = msg.custom_mode
        elif t == "ALTITUDE":
            alt.append(msg.altitude_relative)
        elif t in ("LOGGING_DATA", "LOGGING_DATA_ACKED"):
            log_frame(msg)
        elif t == "STATUSTEXT" and show_text:
            print("      txt: %s" % msg.text.strip())
    if tag:
        a = "last=%.2f max=%.2f" % (alt[-1], max(alt)) if alt else "n/a"
        print("  [%-9s] armed=%-5s mode=0x%08X alt %s" % (tag, armed, mode or 0, a))
    return alt, armed

def cmd(c, *p, name=""):
    p = list(p) + [0] * (7 - len(p))
    m.mav.command_long_send(1, 1, c, 0, *p)
    t0 = time.time()
    while time.time() - t0 < 6:
        keepalive()
        a = m.recv_match(type="COMMAND_ACK", blocking=True, timeout=0.3)
        if a and a.command == c:
            print("  %-20s ack=%d %s" % (name or c, a.result,
                  "OK" if a.result == 0 else "<-- REJECTED")); return a.result
    print("  %-20s NO ACK" % (name or c)); return None

print("== link + joystick keepalive ==")
pump(6, "idle")

print("== leave AUTO.LAND -> AUTO.LOITER ==")
cmd(176, 1, 4, 3, name="SET_MODE AUTO.LOITER")   # base, main=AUTO, sub=LOITER
pump(3, "loiter")

print("== arm, then takeoff IMMEDIATELY ==")
# No pause between the two: PX4 auto-disarms shortly after arming if the land
# detector still says grounded (COM_DISARM_LAND), which is what ended the
# previous attempt at 1.10 m.
cmd(400, 1, 0, name="ARM")
cmd(176, 1, 4, 2, name="SET_MODE AUTO.TAKEOFF")
_, armed = pump(3, "post-arm")
if not armed:
    print("  !! disarmed already - aborting")
else:
    pump(20, "climb")
    print("== ULog stream on (armed, session live) ==")
    cmd(2510, 0, name="LOGGING_START")
    pump(10, "hover1"); print("      frames so far: %d" % len(LOG["chunks"]))
    pump(15, "hover2"); print("      frames so far: %d" % len(LOG["chunks"]))
    print("== land ==")
    cmd(21, name="NAV_LAND")
    pump(30, "descent")
    cmd(400, 0, 0, name="DISARM")
    pump(4, "post-disarm")

pump(2)
print("== ULog stream off ==")
cmd(2511, 0, name="LOGGING_STOP")
pump(2)
write_ulg("/home/petalinux/flight.ulg")
