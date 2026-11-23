#!/usr/bin/env python3
# ============================================================
# mkfat16.py - 在 SaylterOS 裸盘镜像里生成 UEFI ESP（FAT16）
# ------------------------------------------------------------
# 镜像布局（与 build.ps1 / pm.asm 一致）：
#   LBA 0        MBR（boot.asm 代码 + 这里补的分区表）
#   LBA 1..18    stage2 + pm
#   LBA 19..     kernel.bin 暂存区（BIOS 引导用）
#   LBA 3244..   ESP（FAT16，UEFI 引导用）
#   LBA 8192..   SlyzFS 数据区（内核直接按 LBA 读取）
#
# 只写 ESP 区与 MBR 分区表，不动 BIOS 引导代码和 SlyzFS 数据。
# ============================================================

import argparse
import os
import struct
import sys

SECTOR = 512


class Node:
    def __init__(self, name, is_dir, data=b""):
        self.name = name.upper()
        self.is_dir = is_dir
        self.data = data
        self.children = []
        self.first_cluster = 0
        self.size = len(data)


def add_path(root, parts, data):
    node = root
    for p in parts[:-1]:
        found = None
        for c in node.children:
            if c.is_dir and c.name == p.upper():
                found = c
                break
        if found is None:
            found = Node(p, True)
            node.children.append(found)
        node = found
    node.children.append(Node(parts[-1], False, data))


def short_name(name):
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    base = base.upper()[:8]
    ext = ext.upper()[:3]
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def dir_entry(name, attr, cluster, size):
    ent = bytearray(32)
    ent[0:11] = short_name(name)
    ent[11] = attr
    ent[12] = 0
    ent[13] = 0
    # 固定时间：2026-01-01 00:00:00
    dos_date = ((2026 - 1980) << 9) | (1 << 5) | 1
    dos_time = 0
    struct.pack_into("<H", ent, 14, dos_time)
    struct.pack_into("<H", ent, 16, dos_date)
    struct.pack_into("<H", ent, 18, dos_date)
    struct.pack_into("<H", ent, 22, dos_time)
    struct.pack_into("<H", ent, 24, dos_date)
    struct.pack_into("<H", ent, 26, cluster & 0xFFFF)
    struct.pack_into("<I", ent, 28, size)
    return bytes(ent)


def dot_entry(name, cluster):
    ent = bytearray(32)
    ent[0:11] = name.ljust(11).encode("ascii")
    ent[11] = 0x10
    dos_date = ((2026 - 1980) << 9) | (1 << 5) | 1
    struct.pack_into("<H", ent, 16, dos_date)
    struct.pack_into("<H", ent, 18, dos_date)
    struct.pack_into("<H", ent, 24, dos_date)
    struct.pack_into("<H", ent, 26, cluster & 0xFFFF)
    return bytes(ent)


def chs(lba):
    c = lba // (255 * 63)
    h = (lba // 63) % 255
    s = lba % 63 + 1
    if c > 1023:
        return (0xFE, 0xFF, 0xFF)
    return (c & 0xFF, ((c >> 8) & 0x03) | ((s & 0x3F) << 2), h & 0xFF)


def build_fat(root, total_sectors, start_lba):
    root_entries = 512
    root_sectors = root_entries * 32 // SECTOR

    fat_sectors = 1
    fat12 = True
    while True:
        data_sectors = total_sectors - 1 - 2 * fat_sectors - root_sectors
        if data_sectors <= 0:
            raise ValueError("ESP too small")
        clusters = data_sectors  # 1 sector per cluster
        fat12 = clusters < 4085
        if fat12:
            need = ((clusters + 2) * 3 + 1) // 2
        else:
            need = (clusters + 2) * 2
        if need <= fat_sectors * SECTOR:
            break
        fat_sectors += 1

    # 分配簇：目录各占 1 簇，文件按大小连续分配。
    next_cluster = 2
    dir_nodes = []
    files = []

    def alloc(nbytes):
        nonlocal next_cluster
        n = max(1, (nbytes + SECTOR - 1) // SECTOR)
        c = next_cluster
        next_cluster += n
        if next_cluster - 2 > clusters:
            raise ValueError("ESP out of space")
        return c, n

    def assign_dirs(node):
        for ch in node.children:
            if ch.is_dir:
                ch.first_cluster, _ = alloc(SECTOR)
                dir_nodes.append(ch)
                assign_dirs(ch)

    def assign_files(node):
        for ch in node.children:
            if ch.is_dir:
                assign_files(ch)
            else:
                ch.first_cluster, _ = alloc(ch.size)
                files.append(ch)

    assign_dirs(root)
    assign_files(root)

    # ---- FAT 表 ----
    if fat12:
        fat = bytearray(fat_sectors * SECTOR)

        def fat_set(idx, val):
            off = idx + (idx >> 1)
            if idx & 1:
                fat[off] = (fat[off] & 0x0F) | ((val << 4) & 0xF0)
                fat[off + 1] = (val >> 4) & 0xFF
            else:
                fat[off] = val & 0xFF
                fat[off + 1] = (fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)

        fat_set(0, 0xFF8)
        fat_set(1, 0xFFF)
    else:
        fat = [0] * (fat_sectors * SECTOR // 2)
        fat[0] = 0xFFF8
        fat[1] = 0xFFFF

    def chain(first, n):
        if fat12:
            for i in range(n - 1):
                fat_set(first + i, first + i + 1)
            fat_set(first + n - 1, 0xFFF)
            return
        for i in range(n - 1):
            fat[first + i] = first + i + 1
        fat[first + n - 1] = 0xFFFF

    for node in dir_nodes:
        chain(node.first_cluster, 1)
    for ch in files:
        n = max(1, (ch.size + SECTOR - 1) // SECTOR)
        chain(ch.first_cluster, n)

    if fat12:
        fat_bytes = bytes(fat)
    else:
        fat_bytes = bytearray(fat_sectors * SECTOR)
        for i, v in enumerate(fat):
            struct.pack_into("<H", fat_bytes, i * 2, v & 0xFFFF)
        fat_bytes = bytes(fat_bytes)

    # ---- 目录内容 ----
    def dir_bytes(node, parent_cluster):
        out = bytearray()
        if node is not root:
            out += dot_entry(".", node.first_cluster)
            out += dot_entry("..", parent_cluster if parent_cluster else 0)
        for ch in node.children:
            if ch.is_dir:
                out += dir_entry(ch.name, 0x10, ch.first_cluster, 0)
            else:
                out += dir_entry(ch.name, 0x20, ch.first_cluster, ch.size)
        if len(out) > SECTOR:
            raise ValueError("directory too large: " + node.name)
        return bytes(out)

    # ---- 组装分区 ----
    total_bytes = total_sectors * SECTOR
    buf = bytearray(total_bytes)

    # 引导扇区
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"SAYLTER "
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = 1                      # sectors per cluster
    struct.pack_into("<H", bs, 14, 1)   # reserved
    bs[16] = 2                      # FATs
    struct.pack_into("<H", bs, 17, root_entries)
    total16 = total_sectors if total_sectors < 65536 else 0
    struct.pack_into("<H", bs, 19, total16)
    bs[21] = 0xF8
    struct.pack_into("<H", bs, 22, fat_sectors)
    struct.pack_into("<H", bs, 24, 32)   # sectors/track
    struct.pack_into("<H", bs, 26, 64)   # heads
    struct.pack_into("<I", bs, 28, start_lba)
    struct.pack_into("<I", bs, 32, total_sectors)
    bs[36] = 0x80
    bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0x5A594C54)
    bs[43:54] = b"SAYLTEROS  "
    bs[54:62] = b"FAT12   " if fat12 else b"FAT16   "
    bs[510:512] = b"\x55\xAA"
    buf[0:SECTOR] = bs

    buf[SECTOR:SECTOR + len(fat_bytes)] = fat_bytes
    buf[SECTOR * (1 + fat_sectors):SECTOR * (1 + fat_sectors) + len(fat_bytes)] = fat_bytes

    root_off = SECTOR * (1 + 2 * fat_sectors)
    data_start = root_off + root_sectors * SECTOR

    def cluster_off(cl):
        return data_start + (cl - 2) * SECTOR

    root_dir = dir_bytes(root, 0)
    buf[root_off:root_off + len(root_dir)] = root_dir

    def write_dirs(node, parent_cluster):
        if node.is_dir:
            content = dir_bytes(node, parent_cluster)
            off = cluster_off(node.first_cluster)
            buf[off:off + len(content)] = content
            for ch in node.children:
                if ch.is_dir:
                    write_dirs(ch, node.first_cluster)

    for ch in root.children:
        if ch.is_dir:
            write_dirs(ch, 0)

    def write_files(node):
        for ch in node.children:
            if ch.is_dir:
                write_files(ch)
            else:
                off = cluster_off(ch.first_cluster)
                buf[off:off + len(ch.data)] = ch.data

    write_files(root)
    return bytes(buf), fat_sectors, clusters, fat12


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--start", type=int, required=True, help="ESP start LBA")
    ap.add_argument("--sectors", type=int, required=True, help="ESP size in sectors")
    ap.add_argument("--esp-dir", required=True, help="directory tree to place in ESP")
    ap.add_argument("--total-sectors", type=int, default=0, help="whole disk sectors")
    args = ap.parse_args()

    root = Node("/", True)
    base = os.path.abspath(args.esp_dir)
    for dirpath, _dirnames, filenames in os.walk(base):
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, base).replace("\\", "/")
            with open(full, "rb") as f:
                add_path(root, rel.split("/"), f.read())

    if not any(c.name == "EFI" for c in root.children):
        print("ESP: missing EFI directory", file=sys.stderr)
        return 1

    part, fat_sectors, clusters, fat12 = build_fat(root, args.sectors, args.start)

    if not os.path.exists(args.image):
        print("image not found: " + args.image, file=sys.stderr)
        return 1

    disk_sectors = args.total_sectors or (os.path.getsize(args.image) // SECTOR)
    if args.start + args.sectors > disk_sectors:
        print("ESP overlaps end of disk", file=sys.stderr)
        return 1
    if args.start + args.sectors > 6144:
        print("ESP overlaps SlyzFS module area (must end <= LBA 6144)", file=sys.stderr)
        return 1

    with open(args.image, "r+b") as f:
        f.seek(args.start * SECTOR)
        f.write(part)

        # MBR 分区表：1 = ESP，2 = SlyzFS 数据区（从 8192 到盘尾）
        f.seek(446)
        mbr = bytearray(64)

        def put(idx, boot, ptype, lba, count):
            c1, c2, c3 = chs(lba)
            e1, e2, e3 = chs(lba + count - 1)
            off = idx * 16
            mbr[off + 0] = boot
            mbr[off + 1:off + 4] = bytes([e3, e2, e1])
            mbr[off + 4] = ptype
            mbr[off + 5:off + 8] = bytes([c3, c2, c1])
            struct.pack_into("<I", mbr, off + 8, lba)
            struct.pack_into("<I", mbr, off + 12, count)

        put(0, 0x80, 0xEF, args.start, args.sectors)
        fs_start = 8192
        fs_count = disk_sectors - fs_start
        if fs_count > 0:
            put(1, 0x00, 0x7F, fs_start, fs_count)
        f.seek(446)
        f.write(mbr)

    print("ESP: LBA %d..%d (%d sectors), %s %d FAT sectors, %d clusters"
          % (args.start, args.start + args.sectors - 1, args.sectors,
             "FAT12" if fat12 else "FAT16", fat_sectors, clusters))
    return 0


if __name__ == "__main__":
    sys.exit(main())
