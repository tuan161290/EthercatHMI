"""Set the board's clock and RTC to this PC's time, over the USB console (firmware command 'clock').

    python tools/set_clock.py COM3

flash.ps1 runs it after every flash. It waits up to 30 s for the port and the console, so it can
start while the board is still resetting. The board keeps running: the port is opened without
touching the reset lines. Needs pyserial (it is in the ESP-IDF Python environment).

'clock' takes a local time in the board's own time zone (Settings > Time), which need not be the
PC's. So the board's zone is read first from what 'clock' prints, and the PC's UTC time is sent
shifted into it, at the start of a second.
"""
import re
import sys
import time
from datetime import datetime, timedelta, timezone

import serial

WAIT_S = 30
# "2026-10-31 10:34:02 UTC+07:00, RTC present" or "Clock not set (UTC+07:00, RTC present)" (app_console.c)
CLOCK_RE = re.compile(r"(?:(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) |Clock not set \()UTC([+-])(\d\d):(\d\d)\)?, RTC (present|not found)")


def open_port(port, t_end):
    """After a flash the USB device comes back a moment after the reset: retry until it is there."""
    while True:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.2
        s.dtr = False  # no reset
        s.rts = False
        try:
            s.open()
            return s
        except serial.SerialException:
            if time.time() > t_end:
                raise
            time.sleep(0.5)


def command(s, cmd, timeout, seen=None):
    """Send `cmd`; @return the match of the first clock line in the reply, or None.
    The reply lines read on the way are appended to `seen`."""
    s.reset_input_buffer()
    s.write(cmd.encode("ascii") + b"\r\n")
    pending = b""
    t_end = time.time() + timeout
    while time.time() < t_end:
        pending += s.read(4096)
        *lines, pending = pending.split(b"\n")
        for line in lines:
            text = line.decode("ascii", "replace").strip()
            if seen is not None and text:
                seen.append(text)
            m = CLOCK_RE.search(text)
            if m:
                return m
    return None


def board_utc(m, offset):
    """UTC seconds of the time in a clock line, or None if the clock is not set"""
    if m.group(1) is None:
        return None
    local = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
    return (local - timedelta(minutes=offset)).timestamp()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    t_end = time.time() + WAIT_S
    try:
        s = open_port(sys.argv[1], t_end)
    except serial.SerialException as e:
        print("Port %s not available: %s" % (sys.argv[1], e))
        return 1

    m = None
    while m is None and time.time() < t_end:  # the console answers once the firmware is up
        m = command(s, "clock", 1.0)
    if m is None:
        print("No answer to 'clock' within %d s (is the firmware running, and the port free?)" % WAIT_S)
        s.close()
        return 1
    offset = (1 if m.group(2) == "+" else -1) * (int(m.group(3)) * 60 + int(m.group(4)))
    before = board_utc(m, offset)

    # Twice: one of the first tries on 2026-10-31 got no clock line back, cause not found. The reply
    # of a failed try is printed so that a repeat can be traced.
    for attempt in (1, 2):
        now = time.time()
        target = int(now) + 1
        time.sleep(target - now)  # the board takes whole seconds: send at the start of one
        local = datetime.fromtimestamp(target, timezone.utc) + timedelta(minutes=offset)
        seen = []
        m = command(s, "clock " + local.strftime("%Y-%m-%d %H:%M:%S"), 3.0, seen)
        if m is not None and m.group(1) is not None:
            break
        print("Try %d: the board did not take the time. It replied:" % attempt)
        for line in seen:
            print("  " + line)
    s.close()
    if m is None or m.group(1) is None:
        return 1

    zone = "UTC%s%s:%s" % (m.group(2), m.group(3), m.group(4))
    print("Board clock set to %s %s" % (m.group(1), zone))
    if before is None:
        print("  It was not set before.")
    else:
        print("  It was %+.0f s off the PC." % (before - now))
    if m.group(5) != "present":
        print("  No RTC answered: the time is lost at the next reset.")
        return 1
    print("  Written to the RTC as well.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
