# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Read the 8-bit PNG scanout and uncompressed BMP fixtures with the stdlib."""
import struct
import zlib

PNG = b"\x89PNG\r\n\x1a\n"
MAX_PIXELS = 16 * 1024 * 1024
MAX_BYTES = 5 * MAX_PIXELS


def check_size(width, height):
    if not (0 < width <= 8192 and 0 < height <= 8192 and width * height <= MAX_PIXELS):
        raise ValueError("image dimensions out of range")


def paeth(left, above, corner):
    base = left + above - corner
    a, b, c = abs(base - left), abs(base - above), abs(base - corner)
    if a <= b and a <= c:
        return left
    return above if b <= c else corner


def png_rows(raw, stride, channels):
    previous = bytes(stride)
    for start in range(0, len(raw), stride + 1):
        method = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        if method == 1:  # Sub
            for i in range(channels, stride):
                row[i] = (row[i] + row[i - channels]) & 255
        elif method == 2:  # Up
            row = bytes((value + above) & 255 for value, above in zip(row, previous))
        elif method in (3, 4):  # Average or Paeth
            for i in range(channels):
                row[i] = (row[i] + (previous[i] // 2 if method == 3 else previous[i])) & 255
            if method == 3:
                for i in range(channels, stride):
                    row[i] = (row[i] + (row[i - channels] + previous[i]) // 2) & 255
            else:
                for i in range(channels, stride):
                    row[i] = (row[i] + paeth(row[i - channels], previous[i], previous[i - channels])) & 255
        elif method != 0:
            raise ValueError("invalid PNG filter")
        yield row
        previous = row


def read_png(data):
    if len(data) < 33 or data[8:16] != b"\x00\x00\x00\x0dIHDR":
        raise ValueError("missing PNG header")
    width, height, depth, color, compression, filtering, interlaced = struct.unpack_from(">IIBBBBB", data, 16)
    check_size(width, height)
    layouts = {0: "L", 2: "RGB", 4: "LA", 6: "RGBA"}
    if depth != 8 or color not in layouts or compression or filtering or interlaced:
        raise ValueError("unsupported PNG format")

    compressed = bytearray()
    offset = len(PNG)
    ended = False
    view = memoryview(data)
    while offset + 12 <= len(data):
        size, chunk = struct.unpack_from(">I4s", data, offset)
        start, end = offset + 8, offset + 8 + size
        if end + 4 > len(data) or zlib.crc32(view[offset + 4:end]) != struct.unpack_from(">I", data, end)[0]:
            raise ValueError("invalid PNG chunk")
        if chunk == b"IDAT":
            compressed.extend(view[start:end])
        elif chunk == b"IEND":
            ended = size == 0 and end + 4 == len(data)
            break
        elif chunk == b"tRNS" or (chunk[0] & 32 == 0 and chunk not in (b"IHDR", b"PLTE")):
            raise ValueError("unsupported PNG chunk")
        offset = end + 4

    if not ended:
        raise ValueError("incomplete PNG")
    layout = layouts[color]
    stride = width * len(layout)
    expected = (stride + 1) * height
    decoder = zlib.decompressobj()
    raw = decoder.decompress(compressed, expected + 1)
    if len(raw) != expected or not decoder.eof or decoder.unused_data:
        raise ValueError("invalid PNG image data")
    return width, height, layout, png_rows(raw, stride, len(layout))


def read_bmp(data):
    if len(data) < 54:
        raise ValueError("missing BMP header")
    offset, dib, width, signed_height, planes, depth, compression = struct.unpack_from("<IIiiHHI", data, 10)
    height = abs(signed_height)
    check_size(width, height)
    if dib < 40 or offset < 14 + dib or planes != 1 or depth not in (24, 32) or compression:
        raise ValueError("unsupported BMP format")
    stride = ((width * depth + 31) // 32) * 4
    if offset + height * stride > len(data):
        raise ValueError("incomplete BMP")
    order = range(height) if signed_height < 0 else range(height - 1, -1, -1)
    view = memoryview(data)
    return width, height, "BGR" if depth == 24 else "BGRX", (
        view[offset + i * stride:offset + (i + 1) * stride] for i in order
    )


def read_frame(data):
    if isinstance(data, bytes) and len(data) <= MAX_BYTES:
        if data.startswith(PNG):
            return read_png(data)
        if data.startswith(b"BM"):
            return read_bmp(data)
    raise ValueError("missing or unsupported capture")
