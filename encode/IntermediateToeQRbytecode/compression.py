#!/usr/bin/env python3
# String compression
#
# gcc -O3 -fPIC -shared -pthread -o libc2.so c2_exhaustive.c -lm
# gcc -O3 -shared -pthread -static -o c2.dll c2_exhaustive.c -lm
# gcc -O3 -fPIC -shared -pthread -o libc2.dylib c2_exhaustive.c -lm

import ctypes
import os
import platform

EXH_MAX_DEPTH = 1

MIN_LEN_DEFAULT = 2
MAX_LEN_DEFAULT = 32
MAX_DICT_DEFAULT = 1023

FALLBACK_LANGUAGE = "en"

LANGUAGE_IDS = {
    "en": 0,
    "it": 1,
    # dictionaries/languages/<lang>.bin
}

_HERE = os.path.dirname(os.path.abspath(__file__))
DICTIONARIES_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "dictionaries"))
LANGUAGES_DIR = os.path.join(DICTIONARIES_DIR, "languages")

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

class _QRTreeHybridResult(ctypes.Structure):
    _fields_ = [
        ("dict_bits", ctypes.c_char_p),
        ("dict_bits_len", ctypes.c_int32),
        ("stream_bits", ctypes.POINTER(ctypes.c_char_p)),
        ("stream_bits_len", ctypes.POINTER(ctypes.c_int32)),
        ("n_strings", ctypes.c_int32),
    ]

_lib = ctypes.CDLL(_find_library_path())

_lib.qrtree_compress_program_hybrid.restype = ctypes.POINTER(_QRTreeHybridResult)
_lib.qrtree_compress_program_hybrid.argtypes = [
    ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
    ctypes.c_char_p, ctypes.c_int32, ctypes.c_int32,
    ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
]
_lib.qrtree_compress_program_hybrid_free.argtypes = [ctypes.POINTER(_QRTreeHybridResult)]


def _language_dict_path(language: str) -> str:
    return os.path.join(LANGUAGES_DIR, f"{language}.bin")

def _resolve_language(language: str) -> str:
    """Resolves which language to actually use for THIS compression. Fallback only makes sense at encode time (we're free to pick a
    different external alphabet to build against); the decoder has no such freedom -- it must find the exact language dictionary the
    encoder actually used, since its Huffman codes are baked into the bytecode. Never apply this kind of fallback on the decode side."""
    
    if language in LANGUAGE_IDS and os.path.exists(_language_dict_path(language)):
        return language
 
    if language not in LANGUAGE_IDS:
        print(f"note: language '{language}' not recognized (not present in LANGUAGE_IDS), falling back to '{FALLBACK_LANGUAGE}'")
    else:
        print(f"note: dictionary for language '{language}' not found ({_language_dict_path(language)}), falling back to '{FALLBACK_LANGUAGE}'")
 
    if language == FALLBACK_LANGUAGE:
        raise FileNotFoundError(
            f"The fallback language '{FALLBACK_LANGUAGE}' itself is not available: {_language_dict_path(FALLBACK_LANGUAGE)}. "
            f"Build at least one dictionary with build_language_dict.py."
        )
 
    if FALLBACK_LANGUAGE in LANGUAGE_IDS and os.path.exists(_language_dict_path(FALLBACK_LANGUAGE)):
        return FALLBACK_LANGUAGE
 
    raise FileNotFoundError(
        f"No dictionary available: both '{language}' and the fallback '{FALLBACK_LANGUAGE}' are missing "
        f"({_language_dict_path(FALLBACK_LANGUAGE)}). Build at least one dictionary with build_language_dict.py."
    )


def _load_language_dict(language: str):
    """Resolves a language code to (lang_id, dict_bits), falling back to FALLBACK_LANGUAGE if the requested one is unknown or its file is
    missing (see _resolve_language). Reads LANGUAGES_DIR/<resolved_language>.bin."""

    resolved = _resolve_language(language)
    lang_id = LANGUAGE_IDS[resolved]

    with open(_language_dict_path(resolved), "r") as f:
        lang_bits = f.read().strip()

    return lang_id, lang_bits


def compress_program_strings(strings: list, language: str = "en", min_len: int = MIN_LEN_DEFAULT, max_len: int = MAX_LEN_DEFAULT, max_dict: int = MAX_DICT_DEFAULT, nthreads: int = 0) -> dict:
    """Entry point called by myParser.encode(). Runs the whole hybrid pipeline in C, fragment dictionary searched locally on this program's strings, but the alphabet is loaded from the external
    dictionaries/languages/<language>.bin instead of being built from the program and returns:
      - 'dict_bits': lang_id + supplemental alphabet (for any characters
        the language alphabet doesn't cover) + fragments, as ASCII
        '0'/'1' text (NOT including the "10" mode selector, myParser.py
        still writes that, since it owns instruction-level bytecode layout)
      - 'stream_bits': one ASCII '0'/'1' string per input string, in the
        SAME ORDER as `strings`, consumed one-by-one by stringEncoding()
    """

    if nthreads == 0:
        nthreads = os.cpu_count() or 1

    lang_id, lang_bits = _load_language_dict(language)
    lang_bits_bytes = lang_bits.encode('ascii')

    byte_strings = [s.encode('utf-8') for s in strings]
    n = len(byte_strings)

    arr_ptrs = (ctypes.c_char_p * n)(*byte_strings)
    arr_lens = (ctypes.c_int32 * n)(*[len(b) for b in byte_strings])

    res_ptr = _lib.qrtree_compress_program_hybrid(
        arr_ptrs, arr_lens, n,
        lang_bits_bytes, len(lang_bits_bytes), lang_id,
        min_len, max_len, max_dict, EXH_MAX_DEPTH, nthreads,
    )
    res = res_ptr.contents

    dict_bits = res.dict_bits.decode('ascii')
    stream_bits = [res.stream_bits[i].decode('ascii') for i in range(res.n_strings)]

    _lib.qrtree_compress_program_hybrid_free(res_ptr)

    return {
        'dict_bits': dict_bits,
        'stream_bits': stream_bits,
    }