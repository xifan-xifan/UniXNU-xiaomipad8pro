"""Rebuild the archived subtype-2 diagnostic image without touching a device."""
import argparse
import hashlib
from pathlib import Path
import runpy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path, help='Historical outputs directory')
    parser.add_argument('output', type=Path, help='New image path')
    args = parser.parse_args()
    verify = runpy.run_path(str(Path(__file__).with_name('verify-ram-images.py')))
    source = args.archive / 'piano-macos-273-kernel-20261114-fixed-arm64-header'
    inputs = (
        (source / 'boot.img', 'be0327b15e15c21fa30d31f8b8e72ce28df52608b8e8c3752d3310ac65607dca'),
        (source / 'inputs/piano-boot.dtb', '50636effb5f54306ec0ab9a3061cd163d7f003d8797e04585a33205918856840'),
        (args.archive / 'piano-macos-273-esp-test-20261114/payload-subtype2.img',
         'd12691e0beb865e9d3dc8f35662de33f23f593f3c7d5be4eaa494b314cda3b63'),
    )
    blobs = []
    for path, digest in inputs:
        blob = path.read_bytes()
        if hashlib.sha256(blob).hexdigest() != digest:
            parser.error(f'archive hash mismatch: {path}')
        blobs.append(blob)
    original, dtb, payload = blobs
    header = verify['header'](original)
    kernel, _, _, _ = verify['sections'](original, header)
    header.update(recovery_dtbo_size=0, dtb_size=len(dtb),
                  id=verify['mkbootimg_id'](kernel, payload, dtb))
    packed = verify['HEADER'].pack(*(header[name] for name in verify['FIELDS']))
    page = header['page_size']
    image = b''.join(part + bytes(-len(part) % page) for part in (packed, kernel, payload, dtb))
    digest = hashlib.sha256(image).hexdigest()
    if digest != '47125fbd1299240a98ecd234432f0241c53991c296aa55c63d7d3f4c8655b046':
        parser.error(f'unexpected output hash: {digest}')
    with args.output.open('xb') as output:
        output.write(image)
    if hashlib.sha256(args.output.read_bytes()).hexdigest() != digest:
        parser.error('output readback failed')
    print(f'{digest}  {args.output}')


if __name__ == '__main__':
    main()
