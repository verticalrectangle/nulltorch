# NullTorch delta variant ("pth-prime") — deviation spec

This one-page document is handed to the model in the **delta condition**,
alongside delta-encoded fixtures. A converter for stock PyTorch `.pth` files
that is recalled verbatim will fail on these; one grounded in this spec will
pass. Everything not listed here is identical to the stock format.

## Deviation 1 — container signatures

All zip record signatures use `DZ` instead of `PK`:

| record | stock | delta |
|---|---|---|
| local file header | `PK\x03\x04` | `DZ\x03\x04` |
| central directory file header | `PK\x01\x02` | `DZ\x01\x02` |
| end of central directory | `PK\x05\x06` | `DZ\x05\x06` |

All field layouts, sizes, and semantics are unchanged. Entries are STORED.

## Deviation 2 — persistent-ID tuple layout

The pickle persistent ID for a storage is a 5-tuple. Stock order:

    ('storage', <storage_class GLOBAL>, <key str>, <location str>, <numel int>)

Delta order — elements 1 and 2 swapped, and the class is a **plain string**,
not a GLOBAL:

    ('storage', <key str>, <storage_class str>, <location str>, <numel int>)

## Deviation 3 — storage class names

Storage class names end in `Vault` instead of `Storage`:
`FloatVault`, `HalfVault`, `DoubleVault`, `LongVault`, `IntVault`,
`ShortVault`, `CharVault`, `ByteVault`, `BoolVault`, `BFloat16Vault`.
The dtype meanings are unchanged from the stock `…Storage` classes.

## Unchanged (non-exhaustive)

Zip field layouts; `data.pkl` name and location; `data/<key>` storage
records; pickle protocol 2 opcode stream conventions;
`torch._utils._rebuild_tensor_v2(storage, storage_offset, size, stride,
requires_grad, backward_hooks)`; tensor/graph structure; the output
contract (manifest.json + tensors/*.bin) is identical to stock.
