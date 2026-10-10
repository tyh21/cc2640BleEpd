#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
P0-2 多行块写协议 —— 纯主机侧自测 (不需要任何设备/固件)。

验证三件事:
  1) CRC-16/CCITT-FALSE 实现正确 (标准 check 值 = 0x29B1)
  2) 分块 [0x16][off_hi][off_lo][data...] 的偏移与重组逻辑正确
  3) "按块累积 CRC"(固件侧算法) == "整块 CRC"(手机侧算法)
     —— 只要这两者不一致，每一次传图都会"校验失败"，所以必须验证。

运行:  python test_bulk_crc.py
"""

EPD_HEIGHT   = 212
RAM_ROW_SIZE = 16
BLACK_LEN    = EPD_HEIGHT * RAM_ROW_SIZE   # 3392 字节 (212 行 x 16 字节)
CHUNK        = 120                         # 一次块写的字节数
CMD_BULK     = 0x16


def crc16_ccitt(data, crc=0xFFFF):
    """与固件 task_epd.c::crc16_update / 安卓 EpdDownloader::computeBlkCrc 相同。"""
    for b in data:
        crc ^= (b & 0xFF) << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


def build_frames(buf, chunk=CHUNK):
    """模拟客户端: 切成多行块写帧。"""
    frames = []
    off = 0
    while off < len(buf):
        seg = buf[off:off + chunk]
        frames.append(bytes([CMD_BULK, (off >> 8) & 0xFF, off & 0xFF]) + seg)
        off += len(seg)
    return frames


def parse_and_reassemble(frames, total):
    """模拟固件: 按偏移写回 + 按块累积 CRC。"""
    out = bytearray(total)
    crc_dev = 0xFFFF
    for f in frames:
        assert f[0] == CMD_BULK, "bad command byte"
        off = (f[1] << 8) | f[2]
        seg = f[3:]
        out[off:off + len(seg)] = seg
        crc_dev = crc16_ccitt(seg, crc_dev)
    return bytes(out), crc_dev


def main():
    ok = True

    # 1) 标准锚点: CRC-16/CCITT-FALSE("123456789") == 0x29B1
    chk = crc16_ccitt(b"123456789")
    print("1) CRC('123456789') = 0x%04X   (期望 0x29B1)" % chk)
    ok &= (chk == 0x29B1)

    # 造一幅确定性的"图像": 0xFF 底 + 伪随机点 (可复现)
    buf = bytearray([0xFF] * BLACK_LEN)
    x = 0x1234
    for i in range(BLACK_LEN):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        if (x >> 16) & 1:
            buf[i] = (x >> 8) & 0xFF
    buf = bytes(buf)

    frames = build_frames(buf, CHUNK)
    expected_blocks = (BLACK_LEN + CHUNK - 1) // CHUNK
    print("2) 块数 = %d   (旧协议每行一帧 = 212 帧)" % len(frames))
    ok &= (len(frames) == expected_blocks)

    out, crc_dev = parse_and_reassemble(frames, BLACK_LEN)
    same = (out == buf)
    print("3) 重组结果 == 原图 : %s" % same)
    ok &= same

    crc_host = crc16_ccitt(buf)
    print("   整块CRC=0x%04X   逐块累积CRC=0x%04X   一致:%s"
          % (crc_host, crc_dev, crc_host == crc_dev))
    ok &= (crc_host == crc_dev)

    print("\n%s" % ("全部通过 ✅" if ok else "存在失败 ❌"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
