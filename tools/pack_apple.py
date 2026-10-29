"""Convert a video to native 128x296, 1-bit frames with bounded byte RLE."""
import argparse
from pathlib import Path
import struct
import subprocess
from PIL import Image


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


def main():
    p = argparse.ArgumentParser()
    p.add_argument('input'); p.add_argument('output'); p.add_argument('--fps', type=int, default=20)
    a = p.parse_args()
    if not 1 <= a.fps <= 30: p.error('fps must be 1..30')
    filters = (f'fps={a.fps},scale=296:128:force_original_aspect_ratio=decrease,'
               'pad=296:128:(ow-iw)/2:(oh-ih)/2:black,format=gray')
    command = ['ffmpeg', '-v', 'error', '-i', a.input, '-an', '-vf', filters,
               '-f', 'rawvideo', '-pix_fmt', 'gray', 'pipe:1']
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    offsets = [0]; payload = bytearray(); frames = 0; previous = bytes(4736); hashes = []
    key_interval = a.fps
    while True:
        gray = process.stdout.read(296 * 128)
        if not gray: break
        if len(gray) != 296 * 128: raise RuntimeError('Truncated ffmpeg frame')
        image = Image.frombytes('L', (296, 128), gray).point([0] * 128 + [255] * 128, mode='1')
        # GFXcanvas1 rotation 3: native x=y, native y=295-x.
        native = image.transpose(Image.Transpose.ROTATE_90).tobytes()
        delta = native if frames % key_interval == 0 else bytes(x ^ y for x, y in zip(native, previous))
        encoded = pack(delta)
        assert len(native) == 4736 and unpack(encoded) == delta
        previous = native
        checksum = 2166136261
        for value in native: checksum = ((checksum ^ value) * 16777619) & 0xffffffff
        hashes.append(checksum)
        payload.extend(encoded); offsets.append(len(payload)); frames += 1
    if process.wait(): raise RuntimeError('ffmpeg failed')
    if not frames: raise RuntimeError('No video frames')
    header = struct.pack('<4sHHHHI', b'BA29', 128, 296, a.fps, key_interval, frames)
    dest = Path(a.output); dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(header + struct.pack(f'<{len(offsets)}I', *offsets) + payload)
    dest.with_suffix('.hashes').write_bytes(struct.pack(f'<{len(hashes)}I', *hashes))
    print(f'frames={frames} fps={a.fps} duration={frames/a.fps:.2f}s bytes={dest.stat().st_size} '
          f'raw_bytes={frames*4736} ratio={dest.stat().st_size/(frames*4736):.3f}')


if __name__ == '__main__': main()
