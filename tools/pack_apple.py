"""Convert video to native 128x296, 1-bit XOR frames (RLE or zlib)."""
import argparse
from pathlib import Path
import struct
import subprocess
import zlib


def pack(data):
    out = bytearray(); i = 0
    while i < len(data):
        run = 1
        while run < 128 and i + run < len(data) and data[i + run] == data[i]: run += 1
        if run >= 3:
            out.extend((0x80 | (run - 1), data[i])); i += run
        else:
            start = i; i += run
            while i < len(data) and i - start < 128:
                run = 1
                while run < 3 and i + run < len(data) and data[i + run] == data[i]: run += 1
                if run >= 3: break
                i += min(run, 128 - (i - start))
            out.append(i - start - 1); out.extend(data[start:i])
    return bytes(out)


def unpack(data):
    out = bytearray(); i = 0
    while i < len(data):
        control = data[i]; i += 1; length = (control & 127) + 1
        if control & 128:
            out.extend(bytes([data[i]]) * length); i += 1
        else:
            out.extend(data[i:i + length]); i += length
    return bytes(out)


def confirm_two_frames(frames):
    """Reject one-frame pixel excursions using one frame of offline lookahead.

    Persistent changes keep their original time. First and last frames are
    preserved, including a final change for which no future sample exists.
    """
    frames = iter(frames)
    current = next(frames, None)
    if current is None:
        return
    shown = int.from_bytes(current, 'little')
    for following in frames:
        target = int.from_bytes(current, 'little')
        changing = target ^ int.from_bytes(following, 'little')
        shown = (shown & changing) | (target & ~changing)
        yield shown.to_bytes(len(current), 'little')
        current = following
    yield current


def native_frames(stream, content_rows):
    from PIL import Image

    while True:
        gray = stream.read(296 * 128)
        if not gray:
            return
        if len(gray) != 296 * 128:
            raise RuntimeError('Truncated ffmpeg frame')
        image = Image.frombytes('L', (296, 128), gray).point([0] * 128 + [255] * 128, mode='1')
        # GFXcanvas1 rotation 3: native x=y, native y=295-x.
        native = image.transpose(Image.Transpose.ROTATE_90).tobytes()
        if content_rows is not None:
            first, last = content_rows
            # Wire bitmap 1 is white, 0 is black. Remove interpolation leakage
            # into the original side bars, without changing the content area.
            native = bytes(first * 16) + native[first * 16:(last + 1) * 16] + bytes((295 - last) * 16)
        yield native


def main():
    p = argparse.ArgumentParser()
    p.add_argument('input'); p.add_argument('output'); p.add_argument('--fps', type=int, default=20)
    p.add_argument('--codec', choices=['rle', 'deflate'], default='rle')
    p.add_argument('--interpolate', action='store_true',
                   help='Motion-compensated interpolation after scaling, rather than frame duplication')
    p.add_argument('--content-rows', type=int, nargs=2, metavar=('FIRST', 'LAST'),
                   help='Keep this inclusive native row range; restore black bars outside it after interpolation')
    p.add_argument('--temporal-filter', choices=['none', 'confirm2'], default='none',
                   help='Offline two-frame pixel confirmation; keeps FPS and removes single-frame flashes')
    a = p.parse_args()
    if not 1 <= a.fps <= 60: p.error('fps must be 1..60')
    if a.content_rows is not None and not 0 <= a.content_rows[0] <= a.content_rows[1] < 296:
        p.error('content rows must satisfy 0 <= FIRST <= LAST < 296')
    scale = ('scale=296:128:force_original_aspect_ratio=decrease,'
             'pad=296:128:(ow-iw)/2:(oh-ih)/2:black')
    if a.interpolate:
        filters = (scale + ',format=yuv420p,'
                   f'minterpolate=fps={a.fps}:mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1:scd=fdiff,'
                   'format=gray')
    else:
        filters = f'fps={a.fps},' + scale + ',format=gray'
    command = ['ffmpeg', '-v', 'error', '-i', a.input, '-an', '-vf', filters,
               '-f', 'rawvideo', '-pix_fmt', 'gray', 'pipe:1']
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    offsets = [0]; payload = bytearray(); frames = 0; previous = bytes(4736); hashes = []
    key_interval = a.fps
    images = native_frames(process.stdout, a.content_rows)
    if a.temporal_filter == 'confirm2':
        images = confirm_two_frames(images)
    for native in images:
        delta = native if frames % key_interval == 0 else bytes(x ^ y for x, y in zip(native, previous))
        encoded = zlib.compress(delta, 9) if a.codec == 'deflate' else pack(delta)
        decoded = zlib.decompress(encoded) if a.codec == 'deflate' else unpack(encoded)
        assert len(native) == 4736 and decoded == delta
        previous = native
        checksum = 2166136261
        for value in native: checksum = ((checksum ^ value) * 16777619) & 0xffffffff
        hashes.append(checksum)
        payload.extend(encoded); offsets.append(len(payload)); frames += 1
    if process.wait(): raise RuntimeError('ffmpeg failed')
    if not frames: raise RuntimeError('No video frames')
    magic = b'BAZ9' if a.codec == 'deflate' else b'BA29'
    header = struct.pack('<4sHHHHI', magic, 128, 296, a.fps, key_interval, frames)
    dest = Path(a.output); dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(header + struct.pack(f'<{len(offsets)}I', *offsets) + payload)
    dest.with_suffix('.hashes').write_bytes(struct.pack(f'<{len(hashes)}I', *hashes))
    print(f'codec={a.codec} temporal_filter={a.temporal_filter} frames={frames} fps={a.fps} '
          f'duration={frames/a.fps:.2f}s bytes={dest.stat().st_size} '
          f'raw_bytes={frames*4736} ratio={dest.stat().st_size/(frames*4736):.3f}')


if __name__ == '__main__': main()
