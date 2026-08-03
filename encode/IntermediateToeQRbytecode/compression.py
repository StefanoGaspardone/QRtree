#!/usr/bin/env python3
# String compression

# gcc -O3 -fPIC -shared -pthread -o libc2.so c2_exhaustive.c -lm
# gcc -O3 -shared -pthread -static -o c2.dll c2_exhaustive.c -lm
# gcc -O3 -fPIC -shared -pthread -o libc2.dylib c2_exhaustive.c -lm

import ctypes
import os
import platform

ENC_HUFF_LEN = 2
EXH_MAX_DEPTH = 1

MIN_LEN_DEFAULT = 2
MAX_LEN_DEFAULT = 32
MAX_DICT_DEFAULT = 1023

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

def compress_program_strings(strings: list, min_len: int = MIN_LEN_DEFAULT, max_len: int = MAX_LEN_DEFAULT, max_dict: int = MAX_DICT_DEFAULT, nthreads: int = 0) -> dict:
    """Entry point called by myParser.encode(). Runs the whole pipeline in
    C and returns:
      - 'dict_bits': the DICT_LOCAL body as ASCII '0'/'1' text (NOT
        including the "101" command opcode -- myParser.py still writes
        that, since it owns instruction-level bytecode layout)
      - 'stream_bits': one ASCII '0'/'1' string per input string, in the
        SAME ORDER as `strings`, consumed one-by-one by stringEncoding()
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

    return {
        'dict_bits': dict_bits,
        'stream_bits': stream_bits,
    }