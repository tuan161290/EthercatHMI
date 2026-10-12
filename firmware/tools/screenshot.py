"""Save what the HMI screen shows as a PNG, over the USB console (firmware command 'screenshot').

    python tools/screenshot.py COM3 [out.png]

The board keeps running: the port is opened without touching the reset lines. Needs pyserial
(it is in the ESP-IDF Python environment). Format: components/hmi/hmi_screenshot.c.
"""
import base64
import struct
import sys
import time
import zlib

import serial

TIMEOUT_S = 60


def unpack(data, n_pixels):
    """PackBits on 16-bit little-endian pixels, see hmi_screenshot.c."""
    out = bytearray()
    i = 0
    while i < len(data):
        c = data[i]
        i += 1
        if c < 128:
            k = (c + 1) * 2
            out += data[i:i + k]
            i += k
        else:
            out += data[i:i + 2] * (c - 126)
            i += 2
    if len(out) != n_pixels * 2:
        raise ValueError("decoded %d bytes, expected %d" % (len(out), n_pixels * 2))
    return out


def rgb565_to_rgb(raw):
    rgb = bytearray(len(raw) // 2 * 3)
    o = 0
    for (p,) in struct.iter_unpack("<H", raw):
        r, g, b = p >> 11, (p >> 5) & 63, p & 31
        rgb[o] = (r * 527 + 23) >> 6
        rgb[o + 1] = (g * 259 + 33) >> 6
        rgb[o + 2] = (b * 527 + 23) >> 6
        o += 3
    return rgb


def write_png(path, w, h, rgb):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    stride = w * 3
    rows = b"".join(b"\x00" + bytes(rgb[y * stride:(y + 1) * stride]) for y in range(h))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def capture(s):
    """One 'screenshot' command. @return (w, h, data) or an error text"""
    s.reset_input_buffer()
    s.write(b"screenshot\r\n")
    head = None
    parts = {}
    end = None
    pending = b""
    t0 = time.time()
    while end is None and time.time() - t0 < TIMEOUT_S:
        pending += s.read(65536)
        *lines, pending = pending.split(b"\n")
        for line in lines:
            line = line.strip().decode("ascii", "replace")
            if line.startswith("@SNAP begin"):
                f = line.split()
                head = (int(f[2]), int(f[3]), int(f[6]))
                parts.clear()
            elif line.startswith("@S ") and head:
                try:
                    _, off, b64 = line.split(" ", 2)
                    parts[int(off, 16)] = base64.b64decode(b64)
                except ValueError:
                    pass  # a log line ran into this one: caught by the checks below
            elif line.startswith("@SNAP end") and head:
                f = line.split()
                end = (int(f[2]), int(f[3], 16))
            elif line.startswith("No frame buffer") or line.startswith("Out of PSRAM"):
                return line
    if head is None or end is None:
        return "no complete screenshot within %d s (is the firmware running, and the port free?)" % TIMEOUT_S
    w, h, size = head
    data = bytearray()
    for off in sorted(parts):
        if off != len(data):
            return "line at offset 0x%x missing (a log line got in the way)" % len(data)
        data += parts[off]
    if len(data) != size or zlib.adler32(bytes(data)) != end[1]:
        return "checksum mismatch (%d of %d bytes)" % (len(data), size)
    return w, h, data


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    port = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else time.strftime("screenshot_%Y%m%d_%H%M%S.png")

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False  # no reset
    s.rts = False
    s.open()
    t0 = time.time()
    for attempt in range(4):
        r = capture(s)
        if not isinstance(r, str):
            break
        print("attempt %d: %s" % (attempt + 1, r))
    s.close()
    if isinstance(r, str):
        return 1
    w, h, data = r
    write_png(out, w, h, rgb565_to_rgb(unpack(data, w * h)))
    print("Saved %s (%dx%d, %d bytes over the port in %.1f s)" % (out, w, h, len(data), time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
