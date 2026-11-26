# /// script
# requires-python = ">=3.9"
# ///
"""Core2 のシリアルのログから、HCI の記録（hcilog on で出る「@hci」の行）を取り出す。

  uv run tools/hci_log.py LOG                    主なコマンドとイベントに名前を付けて一覧にする
  uv run tools/hci_log.py LOG --btsnoop OUT      Wireshark で開ける btsnoop の形で書く

記録には相手の機器のアドレスが入る。人に渡すときは伏せる。
"""
import argparse
import re
import struct
import sys
from datetime import datetime

COMMANDS = {
    0x0401: "Inquiry", 0x0402: "Inquiry Cancel", 0x0405: "Create Connection", 0x0406: "Disconnect",
    0x0408: "Create Connection Cancel", 0x0409: "Accept Connection Request", 0x040A: "Reject Connection Request",
    0x040B: "Link Key Request Reply", 0x040C: "Link Key Request Negative Reply", 0x040D: "PIN Code Request Reply",
    0x0411: "Authentication Requested", 0x0413: "Set Connection Encryption", 0x0419: "Remote Name Request",
    0x041A: "Remote Name Request Cancel", 0x041B: "Read Remote Supported Features",
    0x041C: "Read Remote Extended Features", 0x041D: "Read Remote Version Information", 0x041F: "Read Clock Offset",
    0x0428: "Setup Synchronous Connection", 0x0429: "Accept Synchronous Connection Request",
    0x042A: "Reject Synchronous Connection Request", 0x042B: "IO Capability Request Reply",
    0x042C: "User Confirmation Request Reply", 0x043D: "Enhanced Setup Synchronous Connection",
    0x043E: "Enhanced Accept Synchronous Connection Request",
    0x0803: "Sniff Mode", 0x0804: "Exit Sniff Mode", 0x0809: "Role Discovery", 0x080B: "Switch Role",
    0x080C: "Read Link Policy Settings", 0x080D: "Write Link Policy Settings",
    0x080F: "Write Default Link Policy Settings", 0x0811: "Sniff Subrating",
    0x0C01: "Set Event Mask", 0x0C03: "Reset", 0x0C05: "Set Event Filter", 0x0C13: "Write Local Name",
    0x0C18: "Write Page Timeout", 0x0C1A: "Write Scan Enable", 0x0C1C: "Write Page Scan Activity",
    0x0C1E: "Write Inquiry Scan Activity", 0x0C24: "Write Class of Device", 0x0C36: "Read Link Supervision Timeout",
    0x0C37: "Write Link Supervision Timeout", 0x0C45: "Write Inquiry Mode", 0x0C52: "Write Extended Inquiry Response",
    0x0C56: "Write Simple Pairing Mode", 0x0C6D: "Write LE Host Support",
    0x0C7A: "Write Secure Connections Host Support",
    0x1001: "Read Local Version Information", 0x1002: "Read Local Supported Commands",
    0x1003: "Read Local Supported Features", 0x1004: "Read Local Extended Features", 0x1005: "Read Buffer Size",
    0x1009: "Read BD_ADDR", 0x1403: "Get Link Quality", 0x1405: "Read RSSI", 0x1406: "Read AFH Channel Map",
}

EVENTS = {
    0x01: "Inquiry Complete", 0x02: "Inquiry Result", 0x03: "Connection Complete", 0x04: "Connection Request",
    0x05: "Disconnection Complete", 0x06: "Authentication Complete", 0x07: "Remote Name Request Complete",
    0x08: "Encryption Change", 0x0B: "Read Remote Supported Features Complete",
    0x0C: "Read Remote Version Information Complete", 0x0E: "Command Complete", 0x0F: "Command Status",
    0x10: "Hardware Error", 0x12: "Role Change", 0x13: "Number of Completed Packets", 0x14: "Mode Change",
    0x16: "PIN Code Request", 0x17: "Link Key Request", 0x18: "Link Key Notification", 0x1B: "Max Slots Change",
    0x1C: "Read Clock Offset Complete", 0x1D: "Connection Packet Type Changed",
    0x20: "Page Scan Repetition Mode Change", 0x22: "Inquiry Result with RSSI",
    0x23: "Read Remote Extended Features Complete", 0x2C: "Synchronous Connection Complete",
    0x2D: "Synchronous Connection Changed", 0x2E: "Sniff Subrating", 0x2F: "Extended Inquiry Result",
    0x30: "Encryption Key Refresh Complete", 0x31: "IO Capability Request", 0x32: "IO Capability Response",
    0x33: "User Confirmation Request", 0x36: "Simple Pairing Complete",
    0x38: "Link Supervision Timeout Changed", 0x3D: "Remote Host Supported Features Notification",
    0x3E: "LE Meta", 0xFF: "Vendor",
}

# 状態の次にハンドル（2 バイト）が来るイベント
EVENTS_WITH_HANDLE = {0x03, 0x05, 0x06, 0x08, 0x0B, 0x0C, 0x14, 0x1C, 0x1D, 0x23, 0x2C, 0x2D, 0x2E, 0x30}

LINE = re.compile(r"^(?:(\S+ \S+) )?@hci (\d+)\.(\d+) ([<>]) (\d+) ([0-9a-f]+)\s*$")


def read(path):
    """(PC の時刻, 起動からの µs, 向き, 元の長さ, バイト列) を返す"""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                continue
            pc, sec, usec, d, n, hexs = m.groups()
            yield pc, int(sec) * 1_000_000 + int(usec), d, int(n), bytes.fromhex(hexs)


def opname(op):
    if op >> 10 == 0x3F:
        return f"Vendor 0x{op:04x}"
    return COMMANDS.get(op, f"0x{op:04x}")


def describe(d, data):
    t = data[0]
    p = data[1:]
    if t == 1 and len(p) >= 3:
        op = p[0] | p[1] << 8
        return f"コマンド {opname(op)} {p[3:].hex()}"
    if t == 4 and len(p) >= 2:
        code, body = p[0], p[2:]
        name = EVENTS.get(code, f"0x{code:02x}")
        if code == 0x0E and len(body) >= 4:
            op = body[1] | body[2] << 8
            return f"イベント {name} {opname(op)} 状態 0x{body[3]:02x} {body[4:].hex()}"
        if code == 0x0F and len(body) >= 4:
            op = body[2] | body[3] << 8
            return f"イベント {name} {opname(op)} 状態 0x{body[0]:02x}"
        if code in EVENTS_WITH_HANDLE and len(body) >= 3:
            handle = (body[1] | body[2] << 8) & 0x0FFF
            return f"イベント {name} 状態 0x{body[0]:02x} ハンドル 0x{handle:03x} {body[3:].hex()}"
        if code == 0x12 and len(body) >= 8:
            return f"イベント {name} 状態 0x{body[0]:02x} 役割 {'親機' if body[7] == 0 else '子機'}"
        return f"イベント {name} {body.hex()}"
    if t == 2 and len(p) >= 4:
        handle = (p[0] | p[1] << 8) & 0x0FFF
        size = p[2] | p[3] << 8
        return f"ACL ハンドル 0x{handle:03x} {size} バイト {p[4:20].hex()}"
    return data.hex()


def to_btsnoop(records, out):
    # btsnoop の時刻は西暦 0 年からの µs。PC の時刻と起動からの時刻の差で、実際の時刻にする
    epoch_0ad = 0x00DCDDB30F2F8000
    base = None
    with open(out, "wb") as f:
        f.write(b"btsnoop\0" + struct.pack(">II", 1, 1002))  # H4
        for pc, us, d, n, data in records:
            if base is None:
                base = (int(datetime.strptime(pc, "%Y-%m-%d %H:%M:%S.%f").timestamp() * 1e6) - us) if pc else 0
            flags = (1 if d == "<" else 0) | (2 if data[0] in (1, 4) else 0)
            f.write(struct.pack(">IIIIq", n, len(data), flags, 0, epoch_0ad + base + us) + data)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("log")
    p.add_argument("--btsnoop", help="btsnoop の形で書くファイル")
    args = p.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    records = list(read(args.log))
    if args.btsnoop:
        to_btsnoop(records, args.btsnoop)
        print(f"{len(records)} 件を書いた: {args.btsnoop}")
        return
    for pc, us, d, n, data in records:
        cut = "" if len(data) == n else f"（{n} バイトのうち {len(data)}）"
        print(f"{us / 1e6:12.6f} {d} {describe(d, data)}{cut}")


if __name__ == "__main__":
    main()
