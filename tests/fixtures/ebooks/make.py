#!/usr/bin/env python3
"""Writes the ebook fixtures for tst_ebooks.cpp: FB2 books (UTF-8,
windows-1251, zipped) and a MOBI with PalmDOC compression, multibyte and
trailing entries on its text records, an EXTH author/title and a cover.
Run it here: python3 make.py"""

import base64
import struct
import zipfile
import zlib

# A 2x2 red PNG for the covers.
def png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + b"\xff\x00\x00" * 2 for _ in range(2))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

COVER = png()

FB2 = """<?xml version="1.0" encoding="{encoding}"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
 <description>
  <title-info>
   <genre>sf</genre>
   <author><first-name>{first}</first-name><middle-name>{middle}</middle-name><last-name>{last}</last-name></author>
   <author><first-name>Second</first-name><last-name>Author</last-name></author>
   <book-title>{title}</book-title>
   <annotation><p>{about}</p><p>Second line.</p></annotation>
   <coverpage><image l:href="#cover.png"/></coverpage>
  </title-info>
 </description>
 <body>
  <title><p>{title}</p></title>
  <section>
   <title><p>{chapter1}</p><p>The Start</p></title>
   <epigraph><p>An epigraph.</p><text-author>Someone</text-author></epigraph>
   <p>{text1} <emphasis>slanted</emphasis> and <strong>bold</strong>.</p>
   <empty-line/>
   <p>A second <a l:href="#n1" type="note">[1]</a> paragraph.</p>
  </section>
  <section>
   <title><p>Part Two</p></title>
   <section>
    <title><p>Chapter 2</p></title>
    <subtitle>A subtitle</subtitle>
    <poem><stanza><v>Line one</v><v>Line two</v></stanza></poem>
   </section>
   <section>
    <p>A section without a title.</p>
   </section>
  </section>
 </body>
 <body name="notes">
  <section id="n1"><title><p>1</p></title><p>The footnote.</p></section>
 </body>
 <binary id="cover.png" content-type="image/png">{cover}</binary>
</FictionBook>
"""

def fb2(encoding, **text):
    cover = base64.encodebytes(COVER).decode()
    return FB2.format(encoding=encoding, cover=cover, **text).encode("utf-8" if encoding == "utf-8" else "cp1251")

english = dict(first="Anna", middle="K.", last="Writer", title="The Test Book", about="A book for the tests.",
               chapter1="Chapter 1", text1="The first paragraph, with")
russian = dict(first="Анна", middle="К.", last="Писатель", title="Тестовая книга", about="Книга для тестов.",
               chapter1="Глава 1", text1="Первый абзац, с буквами ёЁ «кавычками» и")

open("book.fb2", "wb").write(fb2("utf-8", **english))
open("book-1251.fb2", "wb").write(fb2("windows-1251", **russian))
with zipfile.ZipFile("book.fb2.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("book.fb2", fb2("utf-8", **english))

# ------------------------------------------------------------------ MOBI

def palmdoc(data):
    """PalmDOC LZ77: back-references for repeats, space+letter pairs,
    literals (runs of 1-8 bytes for high bytes)."""
    out = bytearray()
    i = 0
    while i < len(data):
        # A back-reference of 3-10 bytes within 2047 bytes.
        best, distance = 0, 0
        for d in range(1, min(i, 2047) + 1):
            n = 0
            while n < 10 and i + n < len(data) and data[i + n] == data[i - d + n]:
                n += 1
            if n > best:
                best, distance = n, d
        if best >= 3:
            pair = 0x8000 | (distance << 3) | (best - 3)
            out += struct.pack(">H", pair)
            i += best
            continue
        c = data[i]
        if c == 0x20 and i + 1 < len(data) and 0x40 <= data[i + 1] <= 0x7F:
            out.append(data[i + 1] ^ 0x80)
            i += 2
        elif c == 0 or 0x09 <= c <= 0x7F:
            out.append(c)
            i += 1
        else:
            run = 1
            while run < 8 and i + run < len(data) and not (data[i + run] == 0 or 0x09 <= data[i + run] <= 0x7F):
                run += 1
            out.append(run)
            out += data[i:i + run]
            i += run
    return bytes(out)

def para(n, words):
    return "<p>" + " ".join(f"{w}{n}" for w in words) + "</p>"

lorem = "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor".split()
chapters = [
    "<html><head><guide></guide></head><body><h1>The MOBI Book</h1><p>By Mo Bi</p>",  # short: goes with the next
    "<h2>Chapter One</h2>" + "".join(para(i, lorem) for i in range(30)) + "<p>Ünïcödé — “quotes” ✓</p>",
    "<h2>Chapter Two</h2>" + "".join(para(i, lorem[::-1]) for i in range(30)),
    "<p>No heading here.</p>" + "".join(para(i, lorem[3:]) for i in range(30)) + "</body></html>",
]
text = "<mbp:pagebreak/>".join(chapters).encode("utf-8")
# A three-byte character across the first record's end, so the
# multibyte overlap is not empty there: a run of x in chapter one up to it.
end_one = len("<mbp:pagebreak/>".join(chapters[:2]).encode("utf-8"))
assert end_one < 4000, end_one
chapters[1] += "<p>" + "x" * (4095 - end_one - 3) + "✓</p>"
text = "<mbp:pagebreak/>".join(chapters).encode("utf-8")
assert text[4095:4098] == "✓".encode(), text[4090:4100]

RECORD = 4096
records = []
for at in range(0, len(text), RECORD):
    chunk = text[at:at + RECORD]
    # Don't cut a UTF-8 character: whatever runs over goes in the
    # multibyte overlap (a copy of the next record's first bytes).
    overlap = b""
    end = at + len(chunk)
    k = end
    while k < len(text) and (text[k] & 0xC0) == 0x80:
        k += 1
    overlap = text[end:k]
    # Trailing entries: multibyte overlap (flag bit 0), then a TBS entry
    # (bit 1) of one byte plus its size.
    records.append(palmdoc(chunk) + overlap + bytes([len(overlap) & 3]) + b"\x07\x82")

title = "The MOBI Book".encode()
exth_records = [(100, "Mo Bi".encode()), (503, "The MOBI Book (Updated)".encode()),
                (103, "<p>A <b>MOBI</b> for the tests.</p>".encode()), (201, struct.pack(">I", 0))]
exth_body = b"".join(struct.pack(">II", t, len(v) + 8) + v for t, v in exth_records)
exth = b"EXTH" + struct.pack(">II", 12 + len(exth_body), len(exth_records)) + exth_body
exth += b"\0" * (-len(exth) % 4)

HEADER = 0xE8
first_image = 1 + len(records)
mobi = bytearray(HEADER)
mobi[0:4] = b"MOBI"
struct.pack_into(">IIII", mobi, 4, HEADER, 2, 65001, 0x1234)  # length, type, encoding, uid
struct.pack_into(">I", mobi, 20, 6)  # file version
full_name_at = 16 + HEADER + len(exth)
struct.pack_into(">II", mobi, 68, full_name_at, len(title))
struct.pack_into(">I", mobi, 92, first_image)
struct.pack_into(">I", mobi, 112, 0x40)  # EXTH present
struct.pack_into(">H", mobi, 0xE2, 0x3)  # extra data flags
palm = struct.pack(">HHIHHHH", 2, 0, len(text), len(records), RECORD, 0, 0)
record0 = palm + bytes(mobi) + exth + title + b"\0" * 2
record0 += b"\0" * (-len(record0) % 4)

all_records = [record0] + records + [COVER, b"\xe9\x8e\r\n"]
offset = 78 + 8 * len(all_records) + 2
pdb = bytearray(b"The_MOBI_Book".ljust(32, b"\0"))
pdb += struct.pack(">HHIIIIII", 0, 0, 0, 0, 0, 0, 0, 0)
pdb += b"BOOKMOBI" + struct.pack(">IIH", 0, 0, len(all_records))
for i, r in enumerate(all_records):
    pdb += struct.pack(">II", offset, i * 2)
    offset += len(r)
pdb += b"\0\0"
for r in all_records:
    pdb += r
open("book.mobi", "wb").write(pdb)
print("text", len(text), "bytes in", len(records), "records")
