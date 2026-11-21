#!/usr/bin/env python3
"""Lay giao dien web tu esp8266_led_controller/web_pages.h ra file thuong cho app Android.

  python3 tools/extract_web.py [thu_muc_dich]

Mac dinh ghi vao android/app/src/main/assets/ :
  app.html  = PAGE_APP  (trang dieu khien)
  ui.css    = PAGE_CSS  (CSS dung chung)
Nhu vay app va firmware luon dung CHUNG mot giao dien, khong phai sua hai noi.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "esp8266_led_controller" / "web_pages.h"
DEFAULT_OUT = ROOT / "android" / "app" / "src" / "main" / "assets"

ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "?": "?", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}


def decode_c_string(body: str) -> bytes:
    """Giai ma phan trong dau nhay cua mot chuoi C (co \\ooo, \\n, \\"...) ra byte UTF-8."""
    out = bytearray()
    i, n = 0, len(body)
    while i < n:
        c = body[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        i += 1
        c = body[i]
        if c in "01234567":                       # toi da 3 chu so bat phan
            j = i
            while j < n and j < i + 3 and body[j] in "01234567":
                j += 1
            out.append(int(body[i:j], 8) & 0xFF)
            i = j
        elif c == "x":
            j = i + 1
            while j < n and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i + 1:j], 16) & 0xFF)
            i = j
        elif c in ESC:
            out += ESC[c].encode("ascii")
            i += 1
        else:
            raise ValueError("escape la: \\" + c)
    return bytes(out)


def read_array(text: str, name: str) -> str:
    m = re.search(r"static const char " + re.escape(name) + r"\[\] PROGMEM =\s*(.*?);\s*(?:\n|$)", text, re.S)
    if not m:
        raise SystemExit("khong thay " + name + " trong web_pages.h")
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    return decode_c_string("".join(parts)).decode("utf-8")


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUT
    out.mkdir(parents=True, exist_ok=True)
    text = HEADER.read_text(encoding="ascii")
    html = read_array(text, "PAGE_APP")
    css = read_array(text, "PAGE_CSS")
    (out / "app.html").write_text(html, encoding="utf-8", newline="")
    (out / "ui.css").write_text(css, encoding="utf-8", newline="")
    print("app.html %d byte, ui.css %d byte -> %s" % (len(html.encode()), len(css.encode()), out))


if __name__ == "__main__":
    main()
