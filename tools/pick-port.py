"""Choose the upload/monitor port by USB identity, not by ttyACM number.

Both boards on the Linux bench enumerate as /dev/ttyACM*, and which number each
gets depends on plug order. With the leader plugged in first, /dev/ttyACM0 is
the 1.46 -- so a hardcoded `upload_port = /dev/ttyACM0` in the badge env means a
bare `pio run -t upload` flashes badge firmware onto the leader. That happened
to be the state of this file before this script existed.

The USB VID:PID cannot be confused: the badge speaks through a CH343 bridge
(1A86:55D3), the leader has no bridge at all and is the S3's own USB
(303A:1001). Set CHORUS_USB_ID in the env to the one this environment wants.
"""
Import("env")

want = env.GetProjectOption("custom_usb_id", None)
if want and not env.subst("$UPLOAD_PORT"):
    from serial.tools import list_ports

    matches = [p.device for p in list_ports.comports() if want.lower() in (p.hwid or "").lower()]
    if len(matches) == 1:
        env.Replace(UPLOAD_PORT=matches[0])
        print("[pick-port] %s -> %s" % (want, matches[0]))
    elif not matches:
        print("[pick-port] no port matching %s attached" % want)
    else:
        # Two identical boards on one bench is a real situation (five badges on
        # the Windows hub). Refuse rather than pick one at random.
        print("[pick-port] %d ports match %s: %s -- pass --upload-port" % (len(matches), want, matches))
