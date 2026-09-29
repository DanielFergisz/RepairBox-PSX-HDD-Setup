"""Recover exact embedded IRX assets from a known ELF, using its symbol table."""
import argparse
import hashlib
from pathlib import Path
import struct

def extract(path, name):
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x01\x01':
        raise ValueError('Expected ELF32 little endian')
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    sections = [struct.unpack_from('<IIIIIIIIII', data, header[6] + i * header[11])
                for i in range(header[12])]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        strings = data[strings[4]:strings[4] + strings[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            key, value, size, info, other, index = struct.unpack_from('<IIIBBH', data, offset)
            key = strings[key:strings.find(b'\0', key)].decode()
            if index > 0 and index < len(sections):
                owner = sections[index]
                start = owner[4] + value - owner[3]
                symbols[key] = data[start:start + size]
    image = symbols[name + '_irx']
    length, = struct.unpack('<I', symbols['size_' + name + '_irx'])
    if len(image) != length or image[:4] != b'\x7fELF':
        raise ValueError('Bad embedded IRX size/signature: ' + name)
    return image

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('elf')
    parser.add_argument('destination')
    parser.add_argument('names', nargs='+')
    args = parser.parse_args()
    dest = Path(args.destination)
    dest.mkdir(parents=True, exist_ok=True)
    for name in args.names:
        image = extract(args.elf, name)
        target = dest / (name + '.irx')
        if target.exists():
            raise FileExistsError(target)
        target.write_bytes(image)
        print(hashlib.sha256(image).hexdigest(), name, len(image))
