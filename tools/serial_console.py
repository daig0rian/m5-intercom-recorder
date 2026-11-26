# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""Core2 のシリアルのログを取り続けながら、コマンドを送って試験を進めるための道具。

  log  PORT LOG       ログを取り続ける（各行の頭に PC の時刻を付ける）。LOG.cmd に書き足された行をコマンドとして送る
  send LOG COMMAND    コマンドを送り、返事（@ok / @err）が来るまで待って、@ で始まる行を出す。
                      --then REGEX で、送ったあとに REGEX に合う行が来るまでも待つ（送る前から見るので取りこぼさない）
  wait LOG REGEX      いまから後に、REGEX に合う行が来るまで待って、その行を出す

log はポートが消えたら（USB を抜いた・電源が切れた）、また現れるのを待って読み続ける。開くときにリセットはしない。
send・wait の終了コード: 0 = @ok・見つかった、1 = @err、2 = 時間切れ。

例（uv で）:
  uv run tools/serial_console.py log COM8 log.txt
  uv run tools/serial_console.py send log.txt "audio off"
  uv run tools/serial_console.py wait log.txt "音声切断|切断されました" --timeout 60
"""
import argparse
import os
import re
import sys
import time
from datetime import datetime


def now():
    return f"{datetime.now():%Y-%m-%d %H:%M:%S.%f}"[:-3]


def cmd_log(args):
    import serial

    inbox = args.log + ".cmd"
    if not os.path.exists(inbox):
        open(inbox, "a", encoding="utf-8").close()
    sent = os.path.getsize(inbox)  # 前からある分は送らない
    end = time.time() + args.seconds if args.seconds else None
    pending = b""

    with open(args.log, "ab") as out:

        def note(text):
            out.write(f"{now()} ### {text}\n".encode("utf-8"))
            out.flush()

        note(f"待ち始め {args.port}")
        while end is None or time.time() < end:
            ser = serial.Serial()
            ser.port = args.port
            ser.baudrate = 115200
            ser.timeout = 0.05
            ser.dtr = False  # 開くときに Core2 をリセットしない
            ser.rts = False
            try:
                ser.open()
            except serial.SerialException:
                time.sleep(0.2)
                continue
            note("開いた")
            try:
                while end is None or time.time() < end:
                    data = ser.read(4096)
                    if data:
                        pending += data
                        *lines, pending = pending.split(b"\n")
                        for line in lines:
                            out.write(now().encode() + b" " + line.rstrip(b"\r") + b"\n")
                        out.flush()
                    size = os.path.getsize(inbox)
                    if size > sent:
                        with open(inbox, "rb") as f:
                            f.seek(sent)
                            chunk = f.read()
                        complete = chunk[: chunk.rfind(b"\n") + 1]  # 書きかけの行は次に回す
                        for line in complete.splitlines():
                            text = line.strip()
                            if text:
                                out.write(f"{now()} >>> ".encode() + text + b"\n")
                                out.flush()
                                ser.write(text + b"\n")
                                time.sleep(0.05)  # Core2 の受信の FIFO（128 バイト）をあふれさせない
                        sent += len(complete)
            except serial.SerialException as e:
                note(f"閉じた ({e.__class__.__name__})")
            finally:
                try:
                    ser.close()
                except Exception:
                    pass
        note("終わり")


def follow(path, start, timeout):
    """start から後に書き足された行を、timeout 秒まで順に返す"""
    deadline = time.time() + timeout
    pos = start
    rest = b""
    while time.time() < deadline:
        size = os.path.getsize(path)
        if size > pos:
            with open(path, "rb") as f:
                f.seek(pos)
                chunk = f.read(size - pos)
            pos = size
            rest += chunk
            *lines, rest = rest.split(b"\n")
            for line in lines:
                yield line.decode("utf-8", "replace")
        else:
            time.sleep(0.05)


def body(line):
    """PC の時刻を除いた、Core2 が出した行"""
    parts = line.split(" ", 2)
    return parts[2] if len(parts) == 3 else line


def cmd_send(args):
    start = os.path.getsize(args.log)
    with open(args.log + ".cmd", "a", encoding="utf-8") as f:
        f.write(args.command + "\n")
    ok, err = f"@ok {args.command}", f"@err {args.command}"
    then = re.compile(args.then) if args.then else None
    seen = False  # 送った印（>>>）より後の行だけを見る
    result = None
    found = then is None
    for line in follow(args.log, start, max(args.timeout, args.then_timeout)):
        text = body(line)
        if text == f">>> {args.command}":
            seen = True
            continue
        if not seen:
            continue
        if not found and then.search(line):
            print(line)
            found = True
        if result is None and text.startswith("@"):
            print(text)
            if text == ok or text.startswith(ok + " "):
                result = 0
            elif text.startswith(err + ":"):
                return 1
        if result is not None and found:
            return result
    if result is None:
        print(f"時間切れ: {args.command}", file=sys.stderr)
    else:
        print(f"時間切れ: {args.then}", file=sys.stderr)
    return 2


def cmd_wait(args):
    pattern = re.compile(args.regex)
    for line in follow(args.log, os.path.getsize(args.log), args.timeout):
        if pattern.search(line):
            print(line)
            return 0
    print(f"時間切れ: {args.regex}", file=sys.stderr)
    return 2


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="mode", required=True)
    a = sub.add_parser("log")
    a.add_argument("port")
    a.add_argument("log")
    a.add_argument("--seconds", type=float, default=0, help="この秒数で終える（0 = ずっと）")
    a = sub.add_parser("send")
    a.add_argument("log")
    a.add_argument("command")
    a.add_argument("--timeout", type=float, default=10)
    a.add_argument("--then", help="送ったあとに、この正規表現に合う行が来るまでも待つ")
    a.add_argument("--then-timeout", type=float, default=60)
    a = sub.add_parser("wait")
    a.add_argument("log")
    a.add_argument("regex")
    a.add_argument("--timeout", type=float, default=60)
    args = p.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    return {"log": cmd_log, "send": cmd_send, "wait": cmd_wait}[args.mode](args) or 0


if __name__ == "__main__":
    sys.exit(main())
