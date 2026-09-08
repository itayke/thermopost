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
import os
import re
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
# booting when this connects still answers instead of timing out. Only ever while
# it is *silent*, though: a second `shot` sent to a board that is already dumping
# queues a second whole frame, which then has nobody to read it.
RESEND_EVERY_S = 2.0
QUIET_S = 0.4

# One garbled line is worth retrying — it is the same word every time, so the
# second refusal is the firmware genuinely not having the command.
UNKNOWN_RETRIES = 1

DRAIN_MAX_S = 5.0   # bound on waiting for a board to stop talking

# The first bytes written after this port is opened are dropped — measured on the
# board, repeatedly: `shot` arrives as `t` and `power` as `ower`, which is what
# produced every "Unrecognized command" this script used to report as old
# firmware. So the first thing sent is a bare newline whose loss costs nothing;
# by the time the real command goes out the link is awake. The re-send loop is
# still the backstop, this just stops it being needed on every single run.
PRIME_S = 0.25

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

# Git-ignored, so a capture is scratch until someone commits it by path.
DEFAULT_DIR = "captures"

# --- Pretending to be a terminal -------------------------------------------
# The board's line editor is linenoise, and in its full (non-dumb) mode it asks
# the terminal where the cursor is — getColumns() does it on every refresh — by
# writing ESC[6n and then reading stdin one byte at a time until an "R" arrives.
# A client that never answers is not merely ignored: those reads eat whatever
# arrives next, so `shot` loses its first characters and the board runs `t` and
# reports an unrecognized command. Answering unblocks it, which is all a real
# terminal is doing here.
CURSOR_QUERY = b"\x1b[6n"
TERMINAL_COLUMNS = 80
CURSOR_REPLY = b"\x1b[1;%dR" % TERMINAL_COLUMNS

# ESC[5n, the probe linenoise sends once at startup to decide between full and
# dumb mode, is deliberately NOT answered: silence picks dumb mode, which has no
# escape sequences and no cursor questions, and is what a script wants.
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


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


def pump(ser, verbose):
    """Read whatever has arrived, answer terminal queries, return whole lines.

    Byte-oriented rather than readline()-based because ESC[6n carries no newline:
    waiting for one would leave the board's line editor blocked on an answer that
    is sitting unread in this end's buffer.
    """
    chunk = ser.read(ser.in_waiting or 1)
    if not chunk:
        return []
    pump.buf += chunk
    while CURSOR_QUERY in pump.buf:
        pump.buf = pump.buf.replace(CURSOR_QUERY, b"", 1)
        ser.write(CURSOR_REPLY)
        ser.flush()
    lines = []
    while b"\n" in pump.buf:
        raw, pump.buf = pump.buf.split(b"\n", 1)
        # Stripped before anything looks at it, and before anything prints it: the
        # board's own output carries colour codes and cursor questions, and echoing
        # those to a real terminal makes *it* answer, into whatever shell comes next.
        line = ANSI.sub(b"", raw).decode("utf-8", "replace").strip()
        if line:
            if verbose:
                print(f"< {line}", file=sys.stderr)
            lines.append(line)
    return lines


pump.buf = b""


def drain(ser):
    """Read until the board stops talking, so it is never left mid-dump.

    Closing the port on a board that is still printing a frame abandons ~200 lines
    with nobody reading them, and the next run then meets a console still working
    through the backlog.
    """
    end = time.monotonic() + DRAIN_MAX_S
    while time.monotonic() < end:
        # Still answering while draining: a board mid-refresh is waiting on one.
        if not pump(ser, False):
            return


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
        pump.buf = b""
        ser.write(b"\r\n")        # sacrificial: an empty line only reprints the prompt
        ser.flush()
        time.sleep(PRIME_S)
        drain(ser)

        deadline = time.monotonic() + CAPTURE_TIMEOUT_S
        next_send = 0.0
        last_rx = 0.0
        unknown = 0
        header = None
        chunks = []
        seen = []
        finished = False
        try:
            while not finished and time.monotonic() < deadline:
                now = time.monotonic()
                # Silence is the precondition, not just the clock: re-sending into a
                # board that is mid-frame queues a dump nobody collects.
                if header is None and now >= next_send and now - last_rx >= QUIET_S:
                    ser.write(b"shot\r\n")
                    ser.flush()
                    next_send = now + RESEND_EVERY_S
                batch = pump(ser, verbose)
                if not batch:
                    continue
                last_rx = time.monotonic()

                for line in batch:
                    # Kept for the error path: a board that answered something is a
                    # very different problem from one that said nothing at all.
                    if len(seen) < DIAGNOSTIC_LINES:
                        seen.append(line)

                    # End and abort are tested before the header, not after: they
                    # share its "# shot " prefix, so the other order reads the last
                    # line of a good capture as the first of a new one.
                    if line.startswith(END_MARKER):
                        finished = True
                        break
                    if line.startswith(ABORT_MARKER):
                        raise RuntimeError("the board aborted the capture mid-frame")
                    if line.startswith(HEADER_PREFIX):
                        header = line[len(HEADER_PREFIX):].split()
                        chunks = []
                    elif line.startswith(DATA_PREFIX) and header is not None:
                        chunks.append(line[len(DATA_PREFIX):])
                    elif UNKNOWN_COMMAND in line:
                        unknown += 1
                        if unknown > UNKNOWN_RETRIES:
                            raise RuntimeError(
                                "the board refused `shot` twice. Either this firmware "
                                "predates the command, or its console is not reading "
                                "input cleanly — power-cycle it and try again.")
                        next_send = 0.0   # one mangled line is not proof
                    # Anything else is skipped on purpose: the REPL echoes the
                    # command, and ESP_LOG shares this one channel, so log lines
                    # land among the pixels and must not be mistaken for them.
        finally:
            # Always, including on the way out of a failure: a board left printing
            # into a closed port is the next run's mystery.
            drain(ser)

    if not finished or header is None:
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
    ap.add_argument("-o", "--out", default=os.path.join(DEFAULT_DIR, "screenshot.png"),
                    help="PNG to write (its directory is created if missing)")
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

    parent = os.path.dirname(args.out)
    if parent:
        os.makedirs(parent, exist_ok=True)
    write_png(args.out, width, height, to_rgb_rows(data, width, height, stride))
    print(f"{args.out}: {width}x{height} from {port}")


if __name__ == "__main__":
    main()
