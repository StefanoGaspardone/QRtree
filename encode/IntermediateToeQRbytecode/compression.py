#!/usr/bin/env python3
# String compression
#
# gcc -O3 -fPIC -shared -pthread -o libc2.so c2_exhaustive.c -lm
# gcc -O3 -shared -pthread -static -o c2.dll c2_exhaustive.c -lm
# gcc -O3 -fPIC -shared -pthread -o libc2.dylib c2_exhaustive.c -lm

import ctypes
import os
import platform

EXH_MAX_DEPTH_DEFAULT = 1

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


class _QRTreeUnifiedResult(ctypes.Structure):
    _fields_ = [
        ("dict_bits", ctypes.c_char_p),
        ("dict_bits_len", ctypes.c_int32),
        ("stream_bits", ctypes.POINTER(ctypes.c_char_p)),
        ("stream_bits_len", ctypes.POINTER(ctypes.c_int32)),
        ("n_strings", ctypes.c_int32),
    ]


_lib = ctypes.CDLL(_find_library_path())

_lib.qrtree_compress_program_unified.restype = ctypes.POINTER(_QRTreeUnifiedResult)
_lib.qrtree_compress_program_unified.argtypes = [
    ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
    ctypes.c_int32,
    ctypes.c_char_p, ctypes.c_int32, ctypes.c_int32,
    ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
]
_lib.qrtree_compress_program_unified_free.argtypes = [ctypes.POINTER(_QRTreeUnifiedResult)]


def _language_dict_path(language: str) -> str:
    return os.path.join(LANGUAGES_DIR, f"{language}.bin")


def _resolve_language(language: str):
    """Resolves which language dictionary to actually use for the compression"""
    
    if language in LANGUAGE_IDS and os.path.exists(_language_dict_path(language)):
        return language

    if language not in LANGUAGE_IDS:
        print(f"note: language '{language}' not recognized (not present in LANGUAGE_IDS)")
    else:
        print(f"note: dictionary for language '{language}' not found ({_language_dict_path(language)})")

    if language != FALLBACK_LANGUAGE and FALLBACK_LANGUAGE in LANGUAGE_IDS and os.path.exists(_language_dict_path(FALLBACK_LANGUAGE)):
        print(f"note: falling back to '{FALLBACK_LANGUAGE}'")
        return FALLBACK_LANGUAGE

    print("note: no language dictionary available (requested and fallback both missing), falling back to fully local mode (no external alphabet)")
    return None


def _load_language_dict(language: str):
    """Resolves a language code and loads its dictionary."""
    
    resolved = _resolve_language(language)
    if resolved is None:
        return None
 
    lang_id = LANGUAGE_IDS[resolved]
 
    with open(_language_dict_path(resolved), "r") as f:
        lang_bits = f.read().strip()
 
    return lang_id, lang_bits


def compress_program_strings(strings: list, language: str = "en", min_len: int = MIN_LEN_DEFAULT, max_len: int = MAX_LEN_DEFAULT, max_dict: int = MAX_DICT_DEFAULT, exh_max_depth: int = EXH_MAX_DEPTH_DEFAULT, nthreads: int = 0) -> dict:
    """Entry point called by myParser.encode(). Runs the whole unified pipeline in C"""
    
    if nthreads == 0:
        nthreads = os.cpu_count() or 1

    lang_dict = _load_language_dict(language)
    has_lang_dict = lang_dict is not None

    if lang_dict is not None:
        lang_id, lang_bits = lang_dict
        lang_bits_bytes = lang_bits.encode('ascii')
        lang_dict_bits_arg = lang_bits_bytes
        lang_dict_bits_len_arg = len(lang_bits_bytes)
        lang_id_arg = lang_id
    else:
        lang_dict_bits_arg = None
        lang_dict_bits_len_arg = 0
        lang_id_arg = 0

    byte_strings = [s.encode('utf-8') for s in strings]
    n = len(byte_strings)
 
    arr_ptrs = (ctypes.c_char_p * n)(*byte_strings)
    arr_lens = (ctypes.c_int32 * n)(*[len(b) for b in byte_strings])
 
    res_ptr = _lib.qrtree_compress_program_unified(
        arr_ptrs, arr_lens, n,
        int(has_lang_dict),
        lang_dict_bits_arg, lang_dict_bits_len_arg, lang_id_arg,
        min_len, max_len, max_dict, exh_max_depth, nthreads,
    )
    res = res_ptr.contents
 
    dict_bits = res.dict_bits.decode('ascii')
    stream_bits = [res.stream_bits[i].decode('ascii') for i in range(res.n_strings)]
 
    _lib.qrtree_compress_program_unified_free(res_ptr)
 
    return {
        'dict_bits': dict_bits,
        'stream_bits': stream_bits,
    }