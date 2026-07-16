# ZIP file format reference

A ZIP archive is a sequence of *local file header + file data* records,
followed by a **central directory** (one header per entry), followed by an
**end of central directory (EOCD)** record. All multi-byte integers are
**little-endian**. Signatures are 4-byte magic values written little-endian,
so they appear in the file as the ASCII bytes `P K x y`.

**A correct reader parses the archive from the back:** locate the EOCD,
follow it to the central directory, and use the central directory as the
authoritative list of entries. Do **not** scan forward for local file
headers — local headers may be stale (their CRC/size fields are zero when a
data descriptor is used), entries may have been superseded by later appends
(orphaned local records that no longer appear in the central directory), and
a `PK\x03\x04` byte pattern can legally occur inside compressed data.

## Local file header — signature `0x04034b50` (`PK\x03\x04`)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Signature `0x04034b50` |
| 4 | 2 | Version needed to extract |
| 6 | 2 | General purpose bit flag |
| 8 | 2 | Compression method |
| 10 | 2 | Last mod file time (MS-DOS format) |
| 12 | 2 | Last mod file date (MS-DOS format) |
| 14 | 4 | CRC-32 of uncompressed data |
| 18 | 4 | Compressed size |
| 22 | 4 | Uncompressed size |
| 26 | 2 | File name length (`n`) |
| 28 | 2 | Extra field length (`m`) |
| 30 | `n` | File name (no NUL terminator, `/` as path separator, no leading `/`) |
| 30+`n` | `m` | Extra field |

Fixed part: **30 bytes**. The file data begins immediately after the extra
field, i.e. at *local-header-offset + 30 + n + m*. If bit 3 of the general
purpose flag is set, the CRC-32 and both size fields are written as **zero**
here and the real values follow the data in a data descriptor — one more
reason to trust the central directory instead.

## Central directory file header — signature `0x02014b50` (`PK\x01\x02`)

One per entry, stored consecutively.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Signature `0x02014b50` |
| 4 | 2 | Version made by |
| 6 | 2 | Version needed to extract |
| 8 | 2 | General purpose bit flag |
| 10 | 2 | Compression method |
| 12 | 2 | Last mod file time |
| 14 | 2 | Last mod file date |
| 16 | 4 | CRC-32 of uncompressed data |
| 20 | 4 | Compressed size |
| 24 | 4 | Uncompressed size |
| 28 | 2 | File name length (`n`) |
| 30 | 2 | Extra field length (`m`) |
| 32 | 2 | File comment length (`k`) |
| 34 | 2 | Disk number where entry starts |
| 36 | 2 | Internal file attributes |
| 38 | 4 | External file attributes (host-dependent; Unix mode in high 16 bits when version-made-by host is 3) |
| 42 | 4 | Relative offset of local file header (from start of archive) |
| 46 | `n` | File name |
| 46+`n` | `m` | Extra field |
| 46+`n`+`m` | `k` | File comment |

Fixed part: **46 bytes**. Any of the 32-bit size/offset fields (and the
16-bit disk fields) may be saturated at `0xFFFFFFFF` (`0xFFFF`), meaning
"real value is in the zip64 extended-information extra field" (below).

## End of central directory (EOCD) — signature `0x06054b50` (`PK\x05\x06`)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Signature `0x06054b50` |
| 4 | 2 | Number of this disk |
| 6 | 2 | Disk where central directory starts |
| 8 | 2 | Number of central directory records on this disk |
| 10 | 2 | Total number of central directory records |
| 12 | 4 | Size of central directory in bytes |
| 16 | 4 | Offset of start of central directory from start of archive |
| 20 | 2 | Archive comment length (`k`) |
| 22 | `k` | Archive comment |

Fixed part: **22 bytes**; it is the last record in the file. Because the
trailing comment may be up to 65535 bytes, locate the EOCD by scanning
**backwards** from end-of-file for the signature within the last
22 + 65535 = 65557 bytes, and validate that the comment length is consistent
with the remaining bytes. For non-multi-disk archives the two disk fields are
0 and the two count fields are equal.

## Zip64

When any EOCD field would overflow (entry count > 65535, central directory
offset or size ≥ 4 GiB), the overflowed EOCD fields are saturated
(`0xFFFF` / `0xFFFFFFFF`) and two extra records precede the EOCD.

### Zip64 end of central directory record — signature `0x06064b50` (`PK\x06\x06`)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Signature `0x06064b50` |
| 4 | 8 | Size of this record, counted **from offset 12** (i.e. total size − 12) |
| 12 | 2 | Version made by |
| 14 | 2 | Version needed to extract (≥ 45 for zip64) |
| 16 | 4 | Number of this disk |
| 20 | 4 | Disk where central directory starts |
| 24 | 8 | Number of central directory records on this disk |
| 32 | 8 | Total number of central directory records |
| 40 | 8 | Size of central directory |
| 48 | 8 | Offset of start of central directory |
| 56 | var | Zip64 extensible data sector (usually empty) |

Fixed part: **56 bytes**.

### Zip64 EOCD locator — signature `0x07064b50` (`PK\x06\x07`)

Sits immediately before the EOCD (fixed 20 bytes):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Signature `0x07064b50` |
| 4 | 4 | Disk containing the zip64 EOCD record |
| 8 | 8 | Offset of the zip64 EOCD record from start of archive |
| 16 | 4 | Total number of disks |

Reader procedure: find the EOCD; if any of its fields are saturated, look for
the locator at *EOCD-offset − 20*, then read the zip64 EOCD at the offset it
gives.

### Zip64 extended-information extra field (header ID `0x0001`)

The extra field area (in both local and central headers) is a sequence of
blocks: 2-byte header ID, 2-byte data size, then that many data bytes. Block
ID `0x0001` carries the 64-bit values for exactly those header fields that
were saturated, in this fixed order (absent fields are omitted):

| Order | Size | Field | Present when header field is |
|---:|---:|---|---|
| 1 | 8 | Uncompressed size | `0xFFFFFFFF` |
| 2 | 8 | Compressed size | `0xFFFFFFFF` |
| 3 | 8 | Offset of local header | `0xFFFFFFFF` (central dir only) |
| 4 | 4 | Disk start number | `0xFFFF` |

In a **local** header's zip64 block, when present, both sizes must appear.

## Data descriptor — optional signature `0x08074b50` (`PK\x07\x08`)

Written immediately after the file data when general-purpose bit 3 is set:

| Size | Field |
|---:|---|
| 4 | Signature `0x08074b50` — **optional**; readers must accept its absence |
| 4 | CRC-32 |
| 4 or 8 | Compressed size (8 bytes iff the entry uses a zip64 extra field) |
| 4 or 8 | Uncompressed size (same width as compressed size) |

A central-directory-driven reader never needs to parse data descriptors: the
central directory always holds the final CRC and sizes.

## General purpose bit flag — relevant bits

| Bit | Meaning |
|---:|---|
| 0 | Entry is encrypted (a plain reader should refuse/skip such entries) |
| 1–2 | For method 8: compressor strategy hint (00 normal, 01 maximum, 10 fast, 11 super-fast). No effect on decoding — ignore |
| 3 | Data descriptor follows the data; CRC/sizes in the **local** header are 0 |
| 11 | Language encoding flag (EFS): file name and comment are UTF-8. When clear, names are nominally IBM code page 437 (ASCII-safe names are unaffected) |
| 13 | Central-directory encryption: selected local-header fields are masked |

All other bits are either method-specific or reserved; a reader may ignore
them but should treat bit 0 and bit 13 as "cannot extract".

## Compression methods

Method field values a general reader must support:

- **0 — STORED.** No transformation: file data is the raw uncompressed bytes;
  compressed size equals uncompressed size.
- **8 — DEFLATE.** File data is a **raw** DEFLATE stream per RFC 1951 — no
  zlib (RFC 1950) header/trailer and no gzip wrapper. Summary below.

Any other method value should produce a structured "unsupported method"
error/skip, never a crash.

### DEFLATE (RFC 1951) summary

A deflate stream is a sequence of blocks. Bits are consumed **LSB-first**
within each byte, *except* that Huffman codes are read most-significant
code bit first. Each block starts with:

- `BFINAL` (1 bit) — set on the last block;
- `BTYPE` (2 bits) — `00` stored, `01` fixed Huffman, `10` dynamic Huffman,
  `11` invalid.

**Stored block (`00`):** skip to the next byte boundary, read `LEN` (2 bytes
LE) and `NLEN` (its one's complement, verify), then copy `LEN` literal bytes.

**Compressed blocks (`01`/`10`):** decode symbols from the literal/length
alphabet until symbol 256 (end of block). Symbols 0–255 are literal bytes.
Symbols 257–285 encode a match length (extra bits below), followed by a
distance symbol 0–29 (5-bit fixed code in fixed blocks, its own Huffman code
in dynamic blocks) plus extra bits; copy `length` bytes from `distance` bytes
back in the output (distance ≤ 32768, may overlap the copy destination).

Length symbols (base length, extra bits): 257–264 → 3..10, 0 bits;
265–268 → 11,13,15,17, 1 bit; 269–272 → 19,23,27,31, 2 bits;
273–276 → 35,43,51,59, 3 bits; 277–280 → 67,83,99,115, 4 bits;
281–284 → 131,163,195,227, 5 bits; 285 → 258, 0 bits.

Distance symbols (base distance, extra bits): 0–3 → 1,2,3,4, 0 bits;
4–5 → 5,7, 1 bit; 6–7 → 9,13, 2 bits; 8–9 → 17,25, 3 bits;
10–11 → 33,49, 4 bits; 12–13 → 65,97, 5 bits; 14–15 → 129,193, 6 bits;
16–17 → 257,385, 7 bits; 18–19 → 513,769, 8 bits;
20–21 → 1025,1537, 9 bits; 22–23 → 2049,3073, 10 bits;
24–25 → 4097,6145, 11 bits; 26–27 → 8193,12289, 12 bits;
28–29 → 16385,24577, 13 bits.

**Fixed Huffman block (`01`):** literal/length code lengths are: symbols
0–143 → 8 bits, 144–255 → 9 bits, 256–279 → 7 bits, 280–287 → 8 bits
(286–287 never occur in data). All 30 distance symbols use plain 5-bit codes.

**Dynamic Huffman block (`10`):** read `HLIT` (5 bits, literal/length code
count − 257), `HDIST` (5 bits, distance code count − 1), `HCLEN` (4 bits,
code-length code count − 4). Then read `HCLEN`+4 3-bit code lengths for the
code-length alphabet, assigned in this fixed symbol order:
`16 17 18 0 8 7 9 6 10 5 11 4 12 3 13 2 14 1 15` (unlisted symbols have
length 0). Build the code-length Huffman code, then decode
`HLIT`+257 + `HDIST`+1 code lengths as one run: symbols 0–15 are literal
lengths; 16 = repeat previous length 3–6 times (2 extra bits); 17 = emit
3–10 zeros (3 extra bits); 18 = emit 11–138 zeros (7 extra bits).

All deflate Huffman codes are **canonical**: shorter codes sort before longer
ones, and within a length, codes are assigned in increasing symbol order
(RFC 1951 §3.2.2 gives the standard construction from the length counts).

## CRC-32

ZIP uses the standard CRC-32 (ISO 3309 / ITU-T V.42 — the same as gzip and
PNG), computed over the **uncompressed** data:

- Generator polynomial `0x04C11DB7`; implemented in reflected (bit-reversed)
  form with constant `0xEDB88320`.
- Initialize the register to `0xFFFFFFFF`; process input LSB-first; XOR the
  final register with `0xFFFFFFFF`.

Byte-at-a-time reflected algorithm:

```
table[n] (n = 0..255): c = n; repeat 8 times: c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0)
crc = 0xFFFFFFFF
for each byte b: crc = (crc >> 8) ^ table[(crc ^ b) & 0xFF]
result = crc ^ 0xFFFFFFFF
```

**Check value:** the CRC-32 of the 9 ASCII bytes `"123456789"` is
`0xCBF43926`. Verify your implementation against this before debugging
anything else.

## MS-DOS time and date fields

Time (2 bytes): bits 0–4 = seconds/2, bits 5–10 = minute, bits 11–15 = hour.
Date (2 bytes): bits 0–4 = day (1–31), bits 5–8 = month (1–12),
bits 9–15 = years since 1980. No time zone is defined.

## Version needed to extract (common values)

`10` (1.0) default · `20` (2.0) deflate or directory entries · `45` (4.5)
zip64. A reader supporting STORED, DEFLATE and zip64 can accept anything
≤ 45 that it otherwise understands.
