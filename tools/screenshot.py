#!/usr/bin/env python3
"""Capture a Thermo Post screen over USB and write it as a PNG.

The firmware's `shot` console command re-renders the active LVGL screen and
prints it as base64 — see the Screenshots section of
components/display/display.cpp. This drives that command and reassembles the
pixels on this end.

    . $HOME/esp/esp-idf/export.sh        # pyserial lives in IDF's environment
    python tools/screenshot.py -o docs/main-screen.png

Close `idf.py monitor` first: the port takes one reader at a time, and a monitor
that already holds it looks like a board that will not answer.

Stdlib only apart from pyserial — the PNG is written by hand with zlib rather
than pulling in Pillow for one file a day.
"""

import argparse
import base64
import binascii
import struct
import sys
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing — run `. $HOME/esp/esp-idf/export.sh` first")

# The ESP32-S3's native USB-Serial-JTAG, which is this board's only port (there
# is no bridge chip; see the Console section of sdkconfig.defaults).
USB_JTAG_VID = 0x303A
USB_JTAG_PID = 0x1001

# Meaningless for USB CDC, which ignores it, but pyserial wants a number.
BAUD = 115200

READ_TIMEOUT_S = 0.5       # per readline, so a silent board is noticed
CAPTURE_TIMEOUT_S = 20.0   # whole attempt; ~200 lines is normally under a second

# Re-sent rather than written once and hoped for, so a board that happens to be
# booting when this connects still answers instead of timing out.
RESEND_EVERY_S = 2.0

# The protocol, matched to the constants in display.cpp.
HEADER_PREFIX = "# shot "
END_MARKER = "# shot end"
ABORT_MARKER = "# shot abort"
DATA_PREFIX = "S:"
UNKNOWN_COMMAND = "Unrecognized command"   # what esp_console prints for a typo
DIAGNOSTIC_LINES = 12                      # enough to show a prompt or a boot log
SUPPORTED_FORMAT = "rgb565"
BYTES_PER_PIXEL = 2

PNG_BIT_DEPTH = 8
PNG_COLOR_TYPE_RGB = 2
PNG_FILTER_NONE = 0
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def find_port():
    """The one ESP32-S3 JTAG port, or None if there isn't exactly one."""
    matches = [p.device for p in list_ports.comports()
               if p.vid == USB_JTAG_VID and p.pid == USB_JTAG_PID]
    return matches[0] if len(matches) == 1 else None


def diagnose(header, seen):
    """Say which of the several ways this fails actually happened."""
    if header is not None:
        return "the frame stopped part-way — try again, and with --verbose"
    if not seen:
        return (f"nothing at all came back in {CAPTURE_TIMEOUT_S:.0f} s. The port opened, "
                "so something is there — check the board is running (not switched off "
                "via the Power page, which deep-sleeps and drops the USB device).")
    heard = "\n".join(f"  {line}" for line in seen)
    return (f"no frame in {CAPTURE_TIMEOUT_S:.0f} s, though the board was talking. "
            f"First lines heard:\n{heard}\n"
            "If there is no `post>` prompt among those, the REPL is not up.")


def capture(port, verbose=False):
    """Run `shot` and return (width, height, stride, pixel bytes)."""
    with serial.Serial(port, BAUD, timeout=READ_TIMEOUT_S) as ser:
        # DTR and RTS are deliberately left exactly as pyserial opened them. On the
        # S3's native USB-Serial-JTAG they are the reset and boot lines, and driving
        # them *is* what reboots the board — measured: with them untouched the post
        # kept running across three reconnects, and deasserting them reset it every
        # time. That matters more than it sounds: a reset would mean this only ever
        # captures the screen the post boots into, and never the one you navigated to.
        ser.reset_input_buffer()

        deadline = time.monotonic() + CAPTURE_TIMEOUT_S
        next_send = 0.0
        header = None
        chunks = []
        seen = []
        while time.monotonic() < deadline:
            now = time.monotonic()
            if header is None and now >= next_send:
                ser.write(b"shot\r\n")
                ser.flush()
                next_send = now + RESEND_EVERY_S
            line = ser.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            if verbose:
                print(f"< {line}", file=sys.stderr)
            # Kept for the error path: a board that answered something is a very
            # different problem from a board that said nothing at all.
            if len(seen) < DIAGNOSTIC_LINES:
                seen.append(line)
            if UNKNOWN_COMMAND in line:
                raise RuntimeError("the board's REPL does not know `shot` — this "
                                   "firmware predates it. Flash the current build.")
            # Everything unmarked is skipped on purpose: the REPL echoes the
            # command, and ESP_LOG shares this one channel, so log lines land in
            # the middle of the pixels and must not be mistaken for them.
            #
            # End and abort are tested before the header, not after: they share its
            # "# shot " prefix, so the other order reads the last line of a good
            # capture as the first line of a new one and throws the frame away.
            if line.startswith(END_MARKER):
                break
            if line.startswith(ABORT_MARKER):
                raise RuntimeError("the board aborted the capture mid-frame")
            if line.startswith(HEADER_PREFIX):
                header = line[len(HEADER_PREFIX):].split()
                chunks = []
            elif line.startswith(DATA_PREFIX) and header is not None:
                chunks.append(line[len(DATA_PREFIX):])
        else:
            raise RuntimeError(diagnose(header, seen))

        if header is None:
            raise RuntimeError(diagnose(header, seen))

    if len(header) != 4:
        raise RuntimeError(f"unreadable header: {' '.join(header)!r}")
    width, height, stride = (int(v) for v in header[:3])
    fmt = header[3]
    if fmt != SUPPORTED_FORMAT:
        raise RuntimeError(f"firmware sent {fmt}, this script only reads {SUPPORTED_FORMAT}")

    # Decoded per line rather than as one joined string: each chunk is a whole
    # number of base64 groups, so a line lost to a collision with a log write
    # costs that line instead of every line after it.
    try:
        data = b"".join(base64.b64decode(c, validate=True) for c in chunks)
    except binascii.Error as e:
        raise RuntimeError(f"corrupt base64 in the stream: {e}") from e

    expected = stride * height
    if len(data) != expected:
        raise RuntimeError(f"got {len(data)} bytes, expected {expected} "
                           f"({width}x{height}, stride {stride})")
    return width, height, stride, data


def to_rgb_rows(data, width, height, stride):
    """RGB565 little-endian to 8-bit RGB rows, ignoring any stride padding.

    The 5- and 6-bit channels are widened by repeating their top bits rather than
    shifting left, so full-scale stays full-scale: 0x1F becomes 0xFF, not 0xF8.
    """
    rows = []
    for y in range(height):
        base = y * stride
        row = bytearray(width * 3)
        for x in range(width):
            lo = data[base + x * BYTES_PER_PIXEL]
            hi = data[base + x * BYTES_PER_PIXEL + 1]
            v = lo | (hi << 8)
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row[x * 3] = (r << 3) | (r >> 2)
            row[x * 3 + 1] = (g << 2) | (g >> 4)
            row[x * 3 + 2] = (b << 3) | (b >> 2)
        rows.append(bytes(row))
    return rows


def write_png(path, width, height, rows):
    def chunk(kind, payload):
        body = kind + payload
        return (struct.pack(">I", len(payload)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    raw = b"".join(bytes([PNG_FILTER_NONE]) + row for row in rows)
    ihdr = struct.pack(">IIBBBBB", width, height, PNG_BIT_DEPTH, PNG_COLOR_TYPE_RGB, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(PNG_MAGIC)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, zlib.Z_BEST_COMPRESSION)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("-o", "--out", default="screenshot.png", help="PNG to write")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="echo every line the board sends, to stderr")
    args = ap.parse_args()

    port = args.port or find_port()
    if port is None:
        listing = "\n".join(f"  {p.device}  {p.description}" for p in list_ports.comports())
        sys.exit("could not pick a port on its own — pass --port. Seen:\n"
                 + (listing or "  (nothing)"))

    try:
        width, height, stride, data = capture(port, args.verbose)
    except serial.SerialException as e:
        sys.exit(f"{port}: {e}\nIs `idf.py monitor` still holding it?")
    except RuntimeError as e:
        sys.exit(str(e))

    write_png(args.out, width, height, to_rgb_rows(data, width, height, stride))
    print(f"{args.out}: {width}x{height} from {port}")


if __name__ == "__main__":
    main()
