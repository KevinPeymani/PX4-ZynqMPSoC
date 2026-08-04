import time
from pymavlink import mavutil
m = mavutil.mavlink_connection("udpout:127.0.0.1:14550", source_system=255)
def hb(): m.mav.heartbeat_send(6,8,0,0,0)
hb()
BITS = {
 0:"3D_GYRO",1:"3D_ACCEL",2:"3D_MAG",3:"ABS_PRESSURE",4:"DIFF_PRESSURE",
 5:"GPS",6:"OPTICAL_FLOW",7:"VISION_POSITION",8:"LASER_POSITION",
 9:"EXTERNAL_GROUND_TRUTH",10:"ANGULAR_RATE_CONTROL",11:"ATTITUDE_STABILIZATION",
 12:"YAW_POSITION",13:"Z_ALTITUDE_CONTROL",14:"XY_POSITION_CONTROL",
 15:"MOTOR_OUTPUTS",16:"RC_RECEIVER",17:"3D_GYRO2",18:"3D_ACCEL2",19:"3D_MAG2",
 20:"GEOFENCE",21:"AHRS",22:"TERRAIN",23:"REVERSE_MOTOR",24:"LOGGING",
 25:"BATTERY",26:"PROXIMITY",27:"SATCOM",28:"PREARM_CHECK",29:"OBSTACLE_AVOIDANCE",
 30:"PROPULSION",
}
t0=time.time()
while time.time()-t0<20:
    hb()
    msg=m.recv_match(type="SYS_STATUS", blocking=True, timeout=1.0)
    if msg is None: continue
    p,e,h = msg.onboard_control_sensors_present, msg.onboard_control_sensors_enabled, msg.onboard_control_sensors_health
    print("present=0x%08X enabled=0x%08X health=0x%08X" % (p,e,h))
    bad=[BITS.get(i,str(i)) for i in range(31) if (e>>i)&1 and not (h>>i)&1]
    miss=[BITS.get(i,str(i)) for i in range(31) if (e>>i)&1 and not (p>>i)&1]
    print("ENABLED BUT UNHEALTHY:", ", ".join(bad) if bad else "(none)")
    print("ENABLED BUT NOT PRESENT:", ", ".join(miss) if miss else "(none)")
    break
