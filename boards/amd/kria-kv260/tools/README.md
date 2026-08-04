# KV260 R5F bring-up tools

Scripts that run **on the board's A53** (PetaLinux side), not on a dev host.
They talk to the R5F through the mavlink-rpmsg bridge on `udpout:127.0.0.1:14550`
and need `pymavlink`, which the bridge already pulls in.

Copy them to `/home/petalinux/`, **not `/tmp`** — `/tmp` is wiped on reboot, and
losing them mid-session twice is why they are versioned here.

| script | what it does |
|---|---|
| `r5cyc.sh` | Stop the bridge, stop the R5, restart the bridge, reload `nuttx.elf`, start the R5. Run it under `systemd-run --unit=…`, never `nohup … &` (systemd kills the session's children at logout). |
| `measure.py` | 25 s sample of `SYS_STATUS.load` plus per-message rates. Load only. |
| `mavsh.py` | MAVLink shell client (`SERIAL_CONTROL`/`DEV_SHELL`). `mavsh.py 'cmd1;;cmd2' <seconds>`. |
| `health.py` | Decodes `SYS_STATUS` sensor bitmasks into "enabled but unhealthy / not present". This is what identifies an arming refusal. |
| `flight.py` | Arm → `AUTO.TAKEOFF` → hover → `NAV_LAND` → disarm, streaming a ULog to `/home/petalinux/flight.ulg`. |

## Things these encode that cost time to learn

**Do not trust CPU load as a metric.** It is stable to 0.1% *within* a boot and
varies by ~8 percentage points *between* boots of identical firmware. Use
`perf`'s minima (`ekf2: EKF update`, `simulator_sih: cycle`) to compare builds;
they are stable to a few microseconds. A load-based A/B will invent wins that
are not there.

**Arming needs `MANUAL_CONTROL`, not a parameter change.** `COM_RC_IN_MODE=1` is
*joystick*, so with no joystick the `RC_RECEIVER` health bit stays bad and
commander answers `Arming denied: Resolve system health failures first`.
`flight.py` sends `MANUAL_CONTROL` at 10 Hz like QGroundControl does, rather
than weakening `COM_RC_IN_MODE`, which was chosen deliberately so QGC's radio
setup page reads as complete.

**Take off immediately after arming.** With a few seconds' gap the land detector
auto-disarms mid-climb (observed at 1.10 m). `flight.py` issues `ARM` and the
mode change back-to-back with no `pump()` between them.

**A ULog stream stalls until you ACK the header.** The first frame arrives as
`LOGGING_DATA_ACKED` and `mavlink_ulog.cpp` blocks the whole stream until the
GCS replies `LOGGING_ACK`, retrying `ACK_MAX_TRIES` then giving up. A client
that ignores it gets exactly one 249-byte frame — which has valid `ULog` magic
and looks like a real log. With the ack: 362 frames / 90 KB / 0 missing.

**One GCS at a time.** The bridge keeps a single `gcs_target` and repoints it at
whoever sent last, so a running QGroundControl and one of these scripts will
fight over the downlink. Use a second `--udp-port` bridge instance, or stop QGC.

## Known bug, open

Sustained ULog streaming **wedges the R5→Linux direction** after roughly 10 s /
360 frames / 90 KB. Signature: the bridge's `r5->gcs` counter freezes while
`gcs->r5` keeps incrementing, with `dropped_oversize=0` and `reconnects=0`, and
the R5 still reporting `running`; every later command gets no ACK. It then
escalates into an RPU state that `remoteproc stop/start` cannot clear ("Failed
to kick remote", no channel announcements) and needs a **full Linux reboot**.
Nothing is dropped and nothing reconnects, which points at the `uart_rpmsg` TX
path rather than the vring. Keep logging windows short until it is fixed.
