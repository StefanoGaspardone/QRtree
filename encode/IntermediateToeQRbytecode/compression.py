#!/usr/bin/env python3
# String compression

# gcc -O3 -fPIC -shared -pthread -o libc2.so c2_exhaustive.c -lm
# gcc -O3 -shared -pthread -static -o c2.dll c2_exhaustive.c -lm
# gcc -O3 -fPIC -shared -pthread -o libc2.dylib c2_exhaustive.c -lm

import ctypes
import datetime
import os
import platform

ENC_HUFF_LEN = 2
EXH_MAX_DEPTH = 1

MIN_LEN_DEFAULT = 2
MAX_LEN_DEFAULT = 32
MAX_DICT_DEFAULT = 1023

_HERE = os.path.dirname(os.path.abspath(__file__))
DICTIONARIES_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "dictionaries"))


def _find_library_path():
    """Locate the compiled c2_exhaustive library next to this file."""

    here = os.path.dirname(os.path.abspath(__file__))
    system = platform.system()

    if system == 'Windows':
        candidates = ['c2.dll', 'libc2.dll']
        build_cmd = "gcc -O3 -shared -pthread -static -o c2.dll c2_exhaustive.c -lm  (MinGW-w64, e.g. MSYS2)"
    elif system == 'Darwin':
        candidates = ['libc2.dylib']
        build_cmd = "gcc -O3 -fPIC -shared -pthread -o libc2.dylib c2_exhaustive.c -lm"
    else:
        candidates = ['libc2.so']
        build_cmd = "gcc -O3 -fPIC -shared -pthread -o libc2.so c2_exhaustive.c -lm"

    for name in candidates:
        path = os.path.join(here, name)

        if os.path.exists(path):
            return path

    raise FileNotFoundError(
        f"Compiled c2_exhaustive library not found next to compression.py "
        f"(looked for: {', '.join(candidates)}). Build it first from "
        f"c2_exhaustive.c, e.g.:\n  {build_cmd}\n"
        f"then place the resulting file in the same folder as compression.py."
    )


class _QRTreeCompressResult(ctypes.Structure):
    _fields_ = [
        ("dict_bits", ctypes.c_char_p),
        ("dict_bits_len", ctypes.c_int32),
        ("stream_bits", ctypes.POINTER(ctypes.c_char_p)),
        ("stream_bits_len", ctypes.POINTER(ctypes.c_int32)),
        ("n_strings", ctypes.c_int32),
    ]


_lib = ctypes.CDLL(_find_library_path())

_lib.qrtree_compress_program.restype = ctypes.POINTER(_QRTreeCompressResult)
_lib.qrtree_compress_program.argtypes = [
    ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
    ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
]
_lib.qrtree_compress_program_free.argtypes = [ctypes.POINTER(_QRTreeCompressResult)]


def _next_dict_id() -> int:
    os.makedirs(DICTIONARIES_DIR, exist_ok=True)
    counter_path = os.path.join(DICTIONARIES_DIR, "counter.txt")

    next_id = 0
    if os.path.exists(counter_path):
        with open(counter_path, "r") as f:
            next_id = int(f.read().strip())

    with open(counter_path, "w") as f:
        f.write(str(next_id + 1))

    return next_id


def _write_manifest_entry(dict_id: int, program_name: str):
    """OPTIONAL, purely for debugging/traceability. Appends one line per
    dict_id to DICTIONARIES_DIR/manifest.txt mapping it back to the
    program it was generated from."""
    manifest_path = os.path.join(DICTIONARIES_DIR, "manifest.txt")
    timestamp = datetime.datetime.now().isoformat(timespec="seconds")

    with open(manifest_path, "a") as f:
        f.write(f"{dict_id}\t{program_name}\t{timestamp}\n")


def compress_program_strings(strings: list, program_name: str, min_len: int = MIN_LEN_DEFAULT, max_len: int = MAX_LEN_DEFAULT, max_dict: int = MAX_DICT_DEFAULT, nthreads: int = 0) -> dict:
    """Entry point called by myParser.encode(). Runs the whole pipeline in
    C, writes the resulting dictionary to DICTIONARIES_DIR/<id>.bin (all
    file I/O and id/manifest bookkeeping happens here, not in the
    scanner/parser), and returns:
      - 'dict_id': the external dictionary's id -- myParser.py only needs
        to write "01" + referenceEncoding(dict_id) as the QRtree header
      - 'stream_bits': one ASCII '0'/'1' string per input string, in the
        SAME ORDER as `strings`, consumed one-by-one by stringEncoding()

    program_name is only used for the manifest entry (traceability), e.g.
    pass the output file's base name.
    """

    if nthreads == 0:
        nthreads = os.cpu_count() or 1

    byte_strings = [s.encode('utf-8') for s in strings]
    n = len(byte_strings)

    arr_ptrs = (ctypes.c_char_p * n)(*byte_strings)
    arr_lens = (ctypes.c_int32 * n)(*[len(b) for b in byte_strings])

    res_ptr = _lib.qrtree_compress_program(
        arr_ptrs, arr_lens, n,
        ENC_HUFF_LEN, min_len, max_len, max_dict, EXH_MAX_DEPTH, nthreads,
    )
    res = res_ptr.contents

    dict_bits = res.dict_bits.decode('ascii')
    stream_bits = [res.stream_bits[i].decode('ascii') for i in range(res.n_strings)]

    _lib.qrtree_compress_program_free(res_ptr)

    dict_id = _next_dict_id()
    _write_manifest_entry(dict_id, program_name)

    with open(os.path.join(DICTIONARIES_DIR, f"{dict_id}.bin"), "w") as dict_file:
        dict_file.write(dict_bits)

    return {
        'dict_id': dict_id,
        'stream_bits': stream_bits,
    }