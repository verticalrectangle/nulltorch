"""NullTorch manifest schema — single source of truth for generator and grader.

STDLIB ONLY. The grader imports this in eval environments that have no torch.

Manifest (manifest.json):
{
  "nulltorch_manifest": 1,
  "byteorder": "little",
  "tensors": {
    "<path>": {
      "dtype": "<token>",          # see DTYPE_TOKENS
      "shape": [int, ...],          # [] for 0-dim
      "stride": [int, ...],         # element strides, torch convention
      "storage_key": "0",           # zip record name data/<key>
      "storage_offset": 0,          # in elements
      "nbytes": 0                   # bytes of contiguous materialization
    }
  }
}

Tensor path rules:
  - Path segments are dict keys (str) or list/tuple indices (decimal str),
    joined with '/'.  Keys may contain dots ("enc.0.weight") but never '/';
    the generator enforces this, making paths unambiguous.
  - Tensor bin filename: path with '/' replaced by '__'.

Strictness: numeric fields must be JSON integers (bool is NOT an int here).
"""

import json
import os

SCHEMA_VERSION = 1

# dtype token -> element size in bytes
DTYPE_TOKENS = {
    "f64": 8, "f32": 4, "f16": 2, "bf16": 2,
    "f8_e4m3": 1, "f8_e5m2": 1,
    "i64": 8, "i32": 4, "i16": 2, "i8": 1, "u8": 1,
    "bool": 1,
}

# torch storage class name -> dtype token (the undocumented layer)
STORAGE_CLASS_DTYPE = {
    "DoubleStorage": "f64",
    "FloatStorage": "f32",
    "HalfStorage": "f16",
    "BFloat16Storage": "bf16",
    "Float8_e4m3fnStorage": "f8_e4m3",
    "Float8_e5m2Storage": "f8_e5m2",
    "LongStorage": "i64",
    "IntStorage": "i32",
    "ShortStorage": "i16",
    "CharStorage": "i8",
    "ByteStorage": "u8",
    "BoolStorage": "bool",
}


def tensor_bin_name(path: str) -> str:
    return path.replace("/", "__") + ".bin"


def _is_int(x) -> bool:
    return isinstance(x, int) and not isinstance(x, bool)


def validate_manifest(obj):
    """Return list of error strings; empty list == valid."""
    errs = []
    if not isinstance(obj, dict):
        return ["manifest root is not an object"]
    if obj.get("nulltorch_manifest") != SCHEMA_VERSION:
        errs.append(f"nulltorch_manifest != {SCHEMA_VERSION}")
    if obj.get("byteorder") != "little":
        errs.append("byteorder != 'little'")
    tensors = obj.get("tensors")
    if not isinstance(tensors, dict):
        return errs + ["tensors is not an object"]
    for path, t in tensors.items():
        p = f"tensors[{path!r}]"
        if "/" in path and path != path.strip("/") or path == "":
            pass  # path content is compared against ground truth anyway
        if not isinstance(t, dict):
            errs.append(f"{p}: not an object")
            continue
        dt = t.get("dtype")
        if dt not in DTYPE_TOKENS:
            errs.append(f"{p}: bad dtype {dt!r}")
        # shape + nbytes are always required; the source-layout fields
        # (stride/storage_key/storage_offset) are optional — an RVC f32
        # conversion emits contiguous output and legitimately omits them.
        if not (isinstance(t.get("shape"), list)
                and all(_is_int(i) for i in t.get("shape", []))):
            errs.append(f"{p}: shape must be a list of ints")
        if not _is_int(t.get("nbytes")):
            errs.append(f"{p}: nbytes must be an int")
        if "stride" in t and not (isinstance(t["stride"], list)
                                  and all(_is_int(i) for i in t["stride"])):
            errs.append(f"{p}: stride must be a list of ints")
        if "storage_key" in t and not isinstance(t["storage_key"], str):
            errs.append(f"{p}: storage_key must be a string")
        if "storage_offset" in t and not _is_int(t["storage_offset"]):
            errs.append(f"{p}: storage_offset must be an int")
        # internal consistency
        if (dt in DTYPE_TOKENS and isinstance(t.get("shape"), list)
                and all(_is_int(i) for i in t.get("shape", []))
                and _is_int(t.get("nbytes"))):
            n = 1
            for d in t["shape"]:
                n *= d
            if t["nbytes"] != n * DTYPE_TOKENS[dt]:
                errs.append(f"{p}: nbytes {t['nbytes']} != "
                            f"prod(shape)*itemsize {n * DTYPE_TOKENS[dt]}")
    # Optional RVC config block (SynthesizerTrnMsNSFsid args + phone_dim).
    if "config" in obj:
        cfg = obj["config"]
        if not isinstance(cfg, dict):
            errs.append("config must be an object")
        else:
            for key in RVC_CONFIG_INT_FIELDS:
                if key in cfg and not _is_int(cfg[key]):
                    errs.append(f"config.{key} must be an int")
    return errs


# RVC config field names (positional args to SynthesizerTrnMsNSFsid) + phone_dim.
RVC_CONFIG_INT_FIELDS = (
    "spec_channels", "segment_size", "inter_channels", "hidden_channels",
    "filter_channels", "n_heads", "n_layers", "kernel_size",
    "upsample_initial_channel", "n_speakers", "gin_channels", "sr", "phone_dim",
)
RVC_CONFIG_FIELDS = RVC_CONFIG_INT_FIELDS + (
    "p_dropout", "resblock", "resblock_kernel_sizes",
    "resblock_dilation_sizes", "upsample_rates", "upsample_kernel_sizes",
)


def canonical_dumps(obj) -> str:
    return json.dumps(obj, sort_keys=True, indent=1, ensure_ascii=False) + "\n"


def load_manifest(path: str):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def storage_partition(manifest) -> dict:
    """storage_key -> sorted list of tensor paths (aliasing equivalence classes)."""
    part = {}
    for path, t in manifest.get("tensors", {}).items():
        part.setdefault(t.get("storage_key"), []).append(path)
    return {k: sorted(v) for k, v in part.items()}
