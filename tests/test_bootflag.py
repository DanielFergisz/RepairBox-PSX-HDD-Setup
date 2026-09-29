#!/usr/bin/env python3
"""Hardware-verified pending-40 bootflag test vector."""

import hashlib

image = bytearray(512)
image[0x14:0x24] = b"BOOTMODE=Normal\0"
image[0x24:0x33] = b"repartition=40\0"
image[0x33:0x3E] = b"contents=1\0"
digest = hashlib.sha1(image[0x14:]).digest()
expected = bytes.fromhex("7EF224EE32261C62050FDCE56D6D2087212503AA")
assert digest == expected
image[:20] = digest
assert len(image) == 512
assert image[0x3E:] == bytes(512 - 0x3E)
assert hashlib.sha1(image[20:]).digest() == expected
print("bootflag generation vector: PASS")
