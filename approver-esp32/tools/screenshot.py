"""Photograph the device's screen over the console and write a PNG.

The device half is the `screenshot` command (CLAUDE.md §10.8, commands.md): it
streams the frame out as base64, a rendered piece at a time, because the panel
cannot be read back and a 480x480 frame does not fit in the C6's SRAM (§10.1).
This is the other half — it drives the port, reassembles the pieces and writes
the file.

    py tools/screenshot.py                       # -> screenshot.png on COM4
    py tools/screenshot.py --port COM7 out.png
    py tools/screenshot.py --from dump.txt out.png   # a capture saved earlier

**Nothing outside the standard library.** Root §1 keeps a short list of approved
dependencies and an image library is not on it, so the PNG is written by hand:
that format is a signature, three chunks and a zlib stream, and `zlib` and
`struct` are in the box. Pillow would be one import and a conversation.

`pyserial` is not in the box, but it is inside ESP-IDF's own venv, which is the
same interpreter working-with-code.md already uses to drive this port. Run this
with that python, or with `--from` and a capture taken any other way.
"""

import argparse
import base64
import re
import struct
import sys
import time
import zlib

# The markers the device prints around the payload. The geometry is in the
# opening one rather than assumed here: a decoder that knows its own answer is a
# decoder that silently produces a sheared image the day a panel changes.
BEGIN = re.compile(r"^-+BEGIN SCREENSHOT (\d+) (\d+) (\w+)-+$")
END = re.compile(r"^-+END SCREENSHOT (\d+) (\d+)-+$")
PIECE = re.compile(r"^@ (-?\d+) (-?\d+) (-?\d+) (-?\d+)$")


class CaptureError(Exception):
    pass


def parse(lines):
    """Text in, (width, height, rgba-less RGB565 rows) out.

    The pieces carry their own coordinates, so they are placed rather than
    concatenated. That costs a few lines here and buys the one thing worth
    buying: a piece that is not the full width still lands where it belongs.
    """
    width = height = None
    fmt = None
    frame = None
    piece = None
    payload = []
    seen_pieces = 0
    claimed = None

    def place(area, data):
        x1, y1, x2, y2 = area
        w = x2 - x1 + 1
        h = y2 - y1 + 1
        want = w * h * 2
        if len(data) != want:
            raise CaptureError(
                "piece %s carries %d bytes, its area needs %d" % (area, len(data), want))
        for row in range(h):
            src = row * w * 2
            dst = ((y1 + row) * width + x1) * 2
            frame[dst:dst + w * 2] = data[src:src + w * 2]

    for line in lines:
        line = line.strip()
        if not line:
            continue

        if width is None:
            found = BEGIN.match(line)
            if found:
                width = int(found.group(1))
                height = int(found.group(2))
                fmt = found.group(3)
                if fmt != "rgb565le":
                    raise CaptureError("unknown pixel format %r" % fmt)
                # Mid-grey rather than zeros, so a piece that never arrived is a
                # visible hole instead of black on a screen that is mostly black.
                frame = bytearray(b"\x08\x42" * (width * height))
            continue

        found = END.match(line)
        if found:
            if piece is not None:
                place(piece, base64.b64decode("".join(payload)))
                seen_pieces += 1
            claimed = (int(found.group(1)), int(found.group(2)))
            break

        found = PIECE.match(line)
        if found:
            if piece is not None:
                place(piece, base64.b64decode("".join(payload)))
                seen_pieces += 1
            piece = tuple(int(g) for g in found.groups())
            payload = []
            continue

        if piece is not None:
            payload.append(line)

    if frame is None:
        raise CaptureError("no screenshot in the input — did the command run?")
    if claimed is None:
        raise CaptureError("the stream stopped before the end marker (%d piece(s) so far)"
                           % seen_pieces)

    pieces, byte_count = claimed
    if pieces != seen_pieces:
        raise CaptureError("the device sent %d piece(s) and %d arrived"
                           % (pieces, seen_pieces))
    print("%d piece(s), %d bytes, %dx%d %s" % (pieces, byte_count, width, height, fmt))
    return width, height, frame


def to_rgb(width, height, frame):
    """RGB565 little-endian to 8-bit RGB.

    The channels are widened by repeating their high bits into the low ones,
    which is what keeps white white — a plain shift left leaves 0xF8 as the
    brightest red there is and tints the whole image.
    """
    out = bytearray(width * height * 3)
    for i in range(width * height):
        value = frame[i * 2] | (frame[i * 2 + 1] << 8)
        r = (value >> 11) & 0x1F
        g = (value >> 5) & 0x3F
        b = value & 0x1F
        out[i * 3] = (r << 3) | (r >> 2)
        out[i * 3 + 1] = (g << 2) | (g >> 4)
        out[i * 3 + 2] = (b << 3) | (b >> 2)
    return out


def write_png(path, width, height, rgb):
    """A PNG in thirty lines, because the alternative is a new dependency.

    Signature, IHDR, one IDAT, IEND. Each scanline is prefixed with filter byte
    0 (none) — filtering would compress a photograph better and this is a screen
    that is mostly flat colour, where it buys almost nothing.
    """

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    raw = bytearray()
    stride = width * 3
    for row in range(height):
        raw.append(0)
        raw += rgb[row * stride:(row + 1) * stride]

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        # 8 bits per channel, colour type 2 (truecolour), no interlace.
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        handle.write(chunk(b"IEND", b""))


def grab(port, timeout):
    """Send `screenshot` and read until the end marker."""
    try:
        import serial  # noqa: PLC0415 - optional, and only on the live path
    except ImportError:
        raise CaptureError(
            "pyserial is not in this interpreter. Use ESP-IDF's venv python "
            "(working-with-code.md names it) or pass --from a saved capture.")

    link = serial.Serial()
    link.port = port
    link.timeout = 0.2
    # **Do not reset the board on open.** The same two lines the pyserial snippet
    # in working-with-code.md carries, and for the same reason: a screenshot that
    # reboots the device photographs the splash.
    link.dtr = False
    link.rts = False
    link.open()
    try:
        link.reset_input_buffer()
        link.write(b"screenshot\r\n")
        link.flush()

        text = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            block = link.read(16384).decode("utf-8", "replace")
            if block:
                text.append(block)
                if "END SCREENSHOT" in block:
                    # One more read, so a marker split across two blocks still
                    # arrives whole.
                    text.append(link.read(4096).decode("utf-8", "replace"))
                    break
        return "".join(text).splitlines()
    finally:
        link.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output", nargs="?", default="screenshot.png")
    parser.add_argument("--port", default="COM4",
                        help="the device console (working-with-code.md: COM4 here)")
    parser.add_argument("--timeout", type=float, default=90.0,
                        help="seconds to wait for the whole frame")
    parser.add_argument("--from", dest="source", metavar="FILE",
                        help="decode a capture saved earlier instead of asking the device")
    args = parser.parse_args()

    try:
        if args.source:
            with open(args.source, "r", encoding="utf-8", errors="replace") as handle:
                lines = handle.read().splitlines()
        else:
            lines = grab(args.port, args.timeout)

        width, height, frame = parse(lines)
        write_png(args.output, width, height, to_rgb(width, height, frame))
    except CaptureError as error:
        print("screenshot failed: %s" % error, file=sys.stderr)
        return 1

    print("wrote %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
