"""Create synthetic 60fps fixtures without downloading or distributing a video."""
from pathlib import Path
import struct
import zlib

root = Path('build/fixtures')
root.mkdir(parents=True, exist_ok=True)
frames = []
for index in range(125):
    image = bytearray([255 if index % 17 == 0 else 0] * 4736)
    for row in range(index % 260, index % 260 + 32):
        image[row * 16 + index % 16] ^= 0xA5
    frames.append(bytes(image))

hashes = []
for image in frames:
    checksum = 2166136261
    for value in image:
        checksum = ((checksum ^ value) * 16777619) & 0xFFFFFFFF
    hashes.append(checksum)

for magic in (b'BA29', b'BAZ9'):
    offsets = [0]
    payload = bytearray()
    for index, image in enumerate(frames):
        delta = image if index % 60 == 0 else bytes(a ^ b for a, b in zip(image, frames[index - 1]))
        if magic == b'BAZ9':
            encoded = zlib.compress(delta)
        else:
            # Literal-only packets deliberately avoid the converter's RLE encoder.
            encoded = b''.join(bytes([len(delta[n:n + 128]) - 1]) + delta[n:n + 128]
                               for n in range(0, len(delta), 128))
        payload.extend(encoded)
        offsets.append(len(payload))
    stem = root / magic.decode().lower()
    stem.with_suffix('.bin').write_bytes(struct.pack('<4sHHHHI', magic, 128, 296, 60, 60, len(frames))
                                        + struct.pack(f'<{len(offsets)}I', *offsets) + payload)
    stem.with_suffix('.hashes').write_bytes(struct.pack(f'<{len(hashes)}I', *hashes))
