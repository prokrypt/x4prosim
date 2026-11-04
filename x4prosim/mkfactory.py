#!/usr/bin/env python3
"""Write the X4 Pro factory partition table and an otadata that boots app0.

usage: mkfactory.py partitions.bin otadata.bin
The X4 Pro ships with this table (freeink-sdk docs/xteink-x4pro-support.md "Partitions"),
not CrossDink's partitions.csv: app slots are 0x7E0000, spiffs only 0x14000. otadata is what
`pio upload` writes at 0xE000 (Arduino boot_app0.bin): entry 0 ota_seq 1 -> ota_0, state unset."""
import hashlib, struct, sys, zlib

TABLE = [  # name, type, subtype, offset, size
    ("nvs", 1, 0x02, 0x9000, 0x5000),
    ("otadata", 1, 0x00, 0xE000, 0x2000),
    ("app0", 0, 0x10, 0x10000, 0x7E0000),
    ("app1", 0, 0x11, 0x7F0000, 0x7E0000),
    ("spiffs", 1, 0x82, 0xFD0000, 0x14000),
    ("coredump", 1, 0x03, 0xFE4000, 0x1C000),
]

t = b"".join(struct.pack("<2sBBII16sI", b"\xaa\x50", ty, st, off, size, name.encode(), 0)
             for name, ty, st, off, size in TABLE)
t += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(t).digest()   # gen_esp32part MD5 entry
with open(sys.argv[1], "wb") as f:
    f.write(t.ljust(0xC00, b"\xff"))

# esp_ota_select_entry_t: ota_seq, seq_label[20], ota_state, crc = crc32_le(UINT32_MAX, ota_seq)
seq = struct.pack("<I", 1)
sel = seq + b"\xff" * 20 + b"\xff" * 4 + struct.pack("<I", zlib.crc32(seq, 0xffffffff))
with open(sys.argv[2], "wb") as f:
    f.write(sel.ljust(0x2000, b"\xff"))
