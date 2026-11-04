#!/usr/bin/env python3
"""The ESP32-S3 AES and SHA accelerators and their GDMA path, against host AES and hashlib.

Drives the registers over qtest (no guest code runs, the CPU is stopped) and compares:
  * typical mode: FIPS-197 appendix C vectors and random blocks, both key sizes, both directions;
  * DMA mode: every block mode, both key sizes, both directions, buffers in internal DRAM and in
    PSRAM (through an MMU page pair that is not contiguous in PSRAM, so one transfer crosses a page),
    split over several descriptors; the IV/counter left behind is what the next call of a stream
    needs (ESP-IDF reads it back), receive descriptors come back CPU-owned with suc_eof on the last;
  * CTR with INC_SEL 0 (INC32, what ESP-IDF's GCM sets): the low 32 bits wrap without a carry;
  * SHA-1/256/512 in DMA mode (what the TLS handshake uses), DMA_START from PSRAM across the page
    split then DMA_CONTINUE from DRAM, against hashlib.

Ported from kycube/x4pro-emu tests/test_aes.py (MIT, (c) 2026 KFloyd.com), rewritten as a plain
script for the x4pro machine; the SHA cases are new.
usage: python3 x4prosim/tests/test_crypto.py [qemu-system-xtensa]   (default: build/, then bin/)"""
import hashlib
import os
import random
import subprocess
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
try:                                     # CFB, CFB8 and OFB moved to `decrepit` in cryptography 47
    from cryptography.hazmat.decrepit.ciphers import modes as old_modes
except ImportError:
    old_modes = modes

AES = 0x6003A000
KEY, TEXT_IN, TEXT_OUT, MODE, TRIGGER, STATE, IV = 0x00, 0x20, 0x30, 0x40, 0x48, 0x4C, 0x50
DMA_ENABLE, BLOCK_MODE, BLOCK_NUM, INC_SEL, DMA_EXIT = 0x90, 0x94, 0x98, 0x9C, 0xB8
IDLE, DONE = 0, 2
BLK_ECB, BLK_CBC, BLK_OFB, BLK_CTR, BLK_CFB8, BLK_CFB128 = range(6)   # AES_BLOCK_MODE
NAMES = ['ecb', 'cbc', 'ofb', 'ctr', 'cfb8', 'cfb128']

SHA = 0x6003B000
SHA_MODE, SHA_DMA_BLOCK_NUM, SHA_DMA_START, SHA_DMA_CONTINUE, SHA_H = 0x00, 0x0C, 0x1C, 0x20, 0x40

GDMA = 0x6003F000
GDMA_PERI_AES, GDMA_PERI_SHA = 6, 7
CH = 2                                   # any channel; the AES finds it by PERI_SEL
IN_REGS, OUT_REGS = GDMA + 0xC0 * CH, GDMA + 0xC0 * CH + 0x60
CONF0, CONF1, LINK, PERI_SEL = 0x00, 0x04, 0x20, 0x48
OUT_AUTO_WRBACK, CHECK_OWNER = 1 << 2, 1 << 12
OUT_LINK_START, IN_LINK_START = 1 << 21, 1 << 22

MMU_TABLE = 0x600C5000
MMU_PSRAM = 1 << 15
DCACHE = 0x3C000000
PAGE = 0x10000

DESC = 0x3FCA0000                        # descriptors: internal DRAM (the 20-bit link field)
DRAM_SRC, DRAM_DST = 0x3FCA1000, 0x3FCA4000
# PSRAM: dcache pages 0x80 and 0x81 mapped to PSRAM pages 3 and 9, buffers straddling the boundary
PSRAM_SRC, PSRAM_DST = DCACHE + 0x81 * PAGE - 0x50, DCACHE + 0x81 * PAGE + 0x2000 - 0x30


def default_qemu():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    for d in ('build', 'bin'):
        p = os.path.join(root, d, 'qemu-system-xtensa')
        if os.path.exists(p):
            return p
    sys.exit('qemu-system-xtensa not built (pass its path)')


class QTest:
    """qemu-system-xtensa -machine x4pro under the qtest accelerator, spoken to over stdio."""

    def __init__(self, qemu, stderr_path):
        self.err = open(stderr_path, 'wb')
        self.p = subprocess.Popen(
            [qemu, '-machine', 'x4pro', '-display', 'none', '-monitor', 'none', '-serial', 'null',
             '-accel', 'qtest', '-qtest', 'stdio', '-qtest-log', os.devnull],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err, text=True, bufsize=1)

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        while True:
            r = self.p.stdout.readline()
            if not r:
                raise AssertionError(f'qtest: QEMU exited on {line!r}')
            if r.startswith(('OK', 'FAIL', 'ERR')):
                break
        assert r.startswith('OK'), f'qtest {line!r} -> {r.strip()}'
        return r[2:].strip()

    def wl(self, addr, val):
        self.cmd(f'writel 0x{addr:x} 0x{val:x}')

    def rl(self, addr):
        return int(self.cmd(f'readl 0x{addr:x}'), 16)

    def write(self, addr, data):
        if data:
            self.cmd(f'write 0x{addr:x} {len(data)} 0x{data.hex()}')

    def read(self, addr, n):
        return bytes.fromhex(self.cmd(f'read 0x{addr:x} {n}')[2:])

    def close(self):
        # A qtest QEMU does not exit when its stdin closes: terminate it.
        self.p.stdin.close()
        self.p.terminate()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait(timeout=5)
        self.err.close()


def host(key, block_mode, iv):
    m = {BLK_ECB: lambda: modes.ECB(), BLK_CBC: lambda: modes.CBC(iv), BLK_OFB: lambda: old_modes.OFB(iv),
         BLK_CTR: lambda: modes.CTR(iv), BLK_CFB8: lambda: old_modes.CFB8(iv),
         BLK_CFB128: lambda: old_modes.CFB(iv)}
    return Cipher(algorithms.AES(key), m[block_mode]())


def mode_reg(key, encrypt):
    return (0 if len(key) == 16 else 2) + (0 if encrypt else 4)


def set_key(q, key):
    k = key.ljust(32, b'\0')
    for i in range(8):
        q.wl(AES + KEY + 4 * i, int.from_bytes(k[4 * i:4 * i + 4], 'little'))


# ---------------------------------------------------------------- typical mode

def typical(q, key, block, encrypt):
    set_key(q, key)
    q.wl(AES + MODE, mode_reg(key, encrypt))
    q.wl(AES + DMA_ENABLE, 0)
    for i in range(4):
        q.wl(AES + TEXT_IN + 4 * i, int.from_bytes(block[4 * i:4 * i + 4], 'little'))
    q.wl(AES + TRIGGER, 1)
    assert q.rl(AES + STATE) == IDLE
    return b''.join(q.rl(AES + TEXT_OUT + 4 * i).to_bytes(4, 'little') for i in range(4))


def test_typical_mode_fips197(q):
    for key, pt, ct in [
        ('000102030405060708090a0b0c0d0e0f', '00112233445566778899aabbccddeeff',
         '69c4e0d86a7b0430d8cdb78070b4c55a'),                                    # FIPS-197 C.1
        ('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
         '00112233445566778899aabbccddeeff', '8ea2b7ca516745bfeafc49904b496089'),  # FIPS-197 C.3
    ]:
        key, pt, ct = bytes.fromhex(key), bytes.fromhex(pt), bytes.fromhex(ct)
        assert typical(q, key, pt, True) == ct
        assert typical(q, key, ct, False) == pt


def test_typical_mode_random_blocks(q):
    for klen in (16, 32):
        rng = random.Random(klen)
        for _ in range(8):
            key, block = rng.randbytes(klen), rng.randbytes(16)
            enc = host(key, BLK_ECB, None).encryptor().update(block)
            assert typical(q, key, block, True) == enc
            assert typical(q, key, enc, False) == block


# ---------------------------------------------------------------- DMA mode

def desc_word(size, length, eof, owner=1):
    return size | (length << 12) | (eof << 30) | (owner << 31)


def chain(q, at, buf, sizes, fill):
    """Descriptors at `at` covering `buf` in pieces of `sizes`; `fill` sets length = size (transmit)."""
    addrs = [at + 12 * i for i in range(len(sizes))]
    off = 0
    for i, (a, n) in enumerate(zip(addrs, sizes)):
        last = i == len(sizes) - 1
        nxt = 0 if last else addrs[i + 1]
        w = desc_word(n, n if fill else 0, int(last and fill))
        q.write(a, w.to_bytes(4, 'little') + (buf + off).to_bytes(4, 'little') + nxt.to_bytes(4, 'little'))
        off += n
    return addrs


def split(total, rng, parts):
    cuts = sorted(rng.sample(range(1, total), parts - 1)) if parts > 1 and total > parts else []
    edges = [0] + cuts + [total]
    return [b - a for a, b in zip(edges, edges[1:])]


def dma(q, key, block_mode, encrypt, iv, data, src, dst, rng, inc_sel=1):
    """One DMA-AES operation the way ESP-IDF's esp_aes_dma_core.c runs it; returns (out, iv after)."""
    n = len(data)
    assert n % 16 == 0
    q.write(src, data)
    q.write(dst, b'\xee' * n)
    out_desc = chain(q, DESC, src, split(n, rng, 3), fill=True)
    in_desc = chain(q, DESC + 0x100, dst, split(n, rng, 2), fill=False)

    q.wl(OUT_REGS + PERI_SEL, GDMA_PERI_AES)
    q.wl(IN_REGS + PERI_SEL, GDMA_PERI_AES)
    q.wl(OUT_REGS + CONF0, OUT_AUTO_WRBACK)
    q.wl(OUT_REGS + CONF1, CHECK_OWNER)
    q.wl(IN_REGS + CONF1, CHECK_OWNER)
    q.wl(OUT_REGS + LINK, (out_desc[0] & 0xFFFFF) | OUT_LINK_START)
    q.wl(IN_REGS + LINK, (in_desc[0] & 0xFFFFF) | IN_LINK_START)

    set_key(q, key)
    q.wl(AES + MODE, mode_reg(key, encrypt))
    q.wl(AES + DMA_ENABLE, 1)
    q.wl(AES + BLOCK_MODE, block_mode)
    q.wl(AES + INC_SEL, inc_sel)
    for i in range(4):
        q.wl(AES + IV + 4 * i, int.from_bytes(iv[4 * i:4 * i + 4], 'little'))
    q.wl(AES + BLOCK_NUM, n // 16)
    q.wl(AES + TRIGGER, 1)
    assert q.rl(AES + STATE) == DONE, 'DMA-AES did not complete (the GDMA could not reach a buffer?)'

    out = q.read(dst, n)
    iv_after = b''.join(q.rl(AES + IV + 4 * i).to_bytes(4, 'little') for i in range(4))
    q.wl(AES + DMA_EXIT, 1)
    assert q.rl(AES + STATE) == IDLE
    q.wl(AES + DMA_ENABLE, 0)

    for i, a in enumerate(in_desc):
        w = int.from_bytes(q.read(a, 4), 'little')
        assert w >> 31 == 0, f'receive descriptor {i} still owned by the GDMA'
        assert (w >> 30) & 1 == (i == len(in_desc) - 1), f'suc_eof on receive descriptor {i}'
    for i, a in enumerate(out_desc):
        assert int.from_bytes(q.read(a, 4), 'little') >> 31 == 0, f'transmit descriptor {i} not written back'
    return out, iv_after


def expected_iv(block_mode, encrypt, iv, data, out):
    """The IV register after the operation: what the next call in the same stream starts from."""
    ct = out if encrypt else data
    if block_mode in (BLK_CBC, BLK_CFB128):
        return ct[-16:]
    if block_mode == BLK_CFB8:
        return (iv + ct)[-16:]
    if block_mode == BLK_OFB:
        return bytes(a ^ b for a, b in zip(out[-16:], data[-16:]))    # the last keystream block
    if block_mode == BLK_CTR:
        return ((int.from_bytes(iv, 'big') + len(data) // 16) % (1 << 128)).to_bytes(16, 'big')
    return None


def test_dma_mode_matches_host_aes(q):
    for block_mode in range(6):
        for klen in (16, 32):
            for where in ('dram', 'psram'):
                tag = f'{NAMES[block_mode]}-{klen * 8}-{where}'
                rng = random.Random(tag)
                src, dst = (DRAM_SRC, DRAM_DST) if where == 'dram' else (PSRAM_SRC, PSRAM_DST)
                for encrypt in (True, False):
                    key, iv, data = rng.randbytes(klen), rng.randbytes(16), rng.randbytes(16 * rng.randint(3, 9))
                    c = host(key, block_mode, iv if block_mode != BLK_ECB else None)
                    want = (c.encryptor() if encrypt else c.decryptor()).update(data)
                    out, iv_after = dma(q, key, block_mode, encrypt, iv, data, src, dst, rng)
                    d = 'enc' if encrypt else 'dec'
                    assert out == want, f'{tag} {d}: output differs from the host AES'
                    exp = expected_iv(block_mode, encrypt, iv, data, out)
                    assert exp is None or iv_after == exp, f'{tag} {d}: IV left for the next call'


def test_dma_stream_continues_from_the_iv_it_leaves(q):
    """Two DMA operations, the second from the IV the first left, equal one host operation."""
    rng = random.Random(7)
    for block_mode in (BLK_CBC, BLK_OFB, BLK_CTR, BLK_CFB8, BLK_CFB128):
        for encrypt in (True, False):
            key, iv = rng.randbytes(32), rng.randbytes(16)
            a, b = rng.randbytes(48), rng.randbytes(80)
            c = host(key, block_mode, iv)
            want = (c.encryptor() if encrypt else c.decryptor()).update(a + b)
            out1, iv1 = dma(q, key, block_mode, encrypt, iv, a, PSRAM_SRC, PSRAM_DST, rng)
            out2, _ = dma(q, key, block_mode, encrypt, iv1, b, DRAM_SRC, DRAM_DST, rng)
            assert out1 + out2 == want, f'{NAMES[block_mode]} {"enc" if encrypt else "dec"}'


def test_dma_ctr_32bit_counter_wraps_without_carry(q):
    """INC_SEL 0: only the low 32 bits of the counter block count, wrapping without a carry."""
    rng = random.Random(32)
    key = rng.randbytes(32)
    iv = bytes.fromhex('00112233445566778899aabb') + (0xFFFFFFFE).to_bytes(4, 'big')
    data = rng.randbytes(64)
    blocks = [iv[:12] + ((0xFFFFFFFE + i) % (1 << 32)).to_bytes(4, 'big') for i in range(4)]
    stream = host(key, BLK_ECB, None).encryptor().update(b''.join(blocks))
    want = bytes(x ^ y for x, y in zip(data, stream))
    for encrypt in (True, False):
        out, iv_after = dma(q, key, BLK_CTR, encrypt, iv, data, PSRAM_SRC, PSRAM_DST, rng, inc_sel=0)
        assert out == want
        assert iv_after == iv[:12] + (2).to_bytes(4, 'big')


# ---------------------------------------------------------------- SHA, DMA mode

def md_pad(msg, blk):
    """FIPS 180-4 padding: 0x80, zeros, then the bit length (8 bytes for 64-byte blocks, 16 for 128)."""
    lenlen = 8 if blk == 64 else 16
    pad = b'\x80' + b'\0' * ((blk - (len(msg) + 1 + lenlen) % blk) % blk)
    return msg + pad + (8 * len(msg)).to_bytes(lenlen, 'big')


def sha_dma(q, op, data, src, rng):
    out_desc = chain(q, DESC, src, split(len(data), rng, 3), fill=True)
    q.write(src, data)
    q.wl(OUT_REGS + PERI_SEL, GDMA_PERI_SHA)
    q.wl(OUT_REGS + CONF0, OUT_AUTO_WRBACK)
    q.wl(OUT_REGS + CONF1, CHECK_OWNER)
    q.wl(OUT_REGS + LINK, (out_desc[0] & 0xFFFFF) | OUT_LINK_START)
    q.wl(SHA + op, 1)
    for i, a in enumerate(out_desc):
        assert int.from_bytes(q.read(a, 4), 'little') >> 31 == 0, f'SHA transmit descriptor {i} not written back'


def test_sha_dma_matches_hashlib(q):
    for mode, name, blk, dlen in ((0, 'sha1', 64, 20), (2, 'sha256', 64, 32), (4, 'sha512', 128, 64)):
        rng = random.Random(name)
        msg = rng.randbytes(rng.randint(5 * blk, 9 * blk))
        padded = md_pad(msg, blk)
        first = 2 * blk                                    # DMA_START: two blocks, then DMA_CONTINUE
        q.wl(SHA + SHA_MODE, mode)
        q.wl(SHA + SHA_DMA_BLOCK_NUM, first // blk)
        sha_dma(q, SHA_DMA_START, padded[:first], PSRAM_SRC, rng)
        q.wl(SHA + SHA_DMA_BLOCK_NUM, (len(padded) - first) // blk)
        sha_dma(q, SHA_DMA_CONTINUE, padded[first:], DRAM_SRC, rng)
        got = b''.join(q.rl(SHA + SHA_H + 4 * i).to_bytes(4, 'little') for i in range(dlen // 4))
        assert got == hashlib.new(name, msg).digest(), f'{name}: digest differs from hashlib'


def main():
    qemu = sys.argv[1] if len(sys.argv) > 1 else default_qemu()
    err = f'/tmp/x4prosim-crypto-{os.getpid()}.log'
    q = QTest(qemu, err)
    failed = 0
    try:
        q.wl(MMU_TABLE + 4 * 0x80, MMU_PSRAM | 3)
        q.wl(MMU_TABLE + 4 * 0x81, MMU_PSRAM | 9)
        for name, fn in list(globals().items()):
            if name.startswith('test_'):
                try:
                    fn(q)
                    print(f'ok   {name}')
                except AssertionError as e:
                    failed += 1
                    print(f'FAIL {name}: {e}')
    finally:
        q.close()
    log = open(err, errors='replace').read()
    os.unlink(err)
    if '[AES]' in log or '[SHA]' in log or 'GDMA' in log:
        failed += 1
        print('FAIL qemu stderr:\n' + log)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
