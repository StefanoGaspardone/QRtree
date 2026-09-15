#!/usr/bin/env python3
# String compression
#
# gcc -O3 -fPIC -shared -pthread -o libc2.so c2_exhaustive.c -lm
# gcc -O3 -shared -pthread -static -o c2.dll c2_exhaustive.c -lm
# gcc -O3 -fPIC -shared -pthread -o libc2.dylib c2_exhaustive.c -lm

import ctypes
import importlib.util
import os
import platform

EXH_MAX_DEPTH_DEFAULT = 1

MIN_LEN_DEFAULT = 2
MAX_LEN_DEFAULT = 32
MAX_DICT_DEFAULT = 1023

FALLBACK_LANGUAGE = "en"

_HERE = os.path.dirname(os.path.abspath(__file__))
DICTIONARIES_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "dictionaries"))
LANGUAGES_DIR = DICTIONARIES_DIR


def _load_language_ids() -> dict:
    """Loads the single shared LANGUAGE_IDS mapping from dictionaries/lang_ids.py"""

    path = os.path.join(DICTIONARIES_DIR, "lang_ids.py")

    if not os.path.exists(path):
        raise FileNotFoundError(
            f"Shared language id mapping not found: {path}. "
            f"This file is the single source of truth for LANGUAGE_IDS, shared with decompression.py."
        )

    spec = importlib.util.spec_from_file_location("qrtree_lang_ids", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"Could not load module spec from {path}")

    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    return mod.LANGUAGE_IDS


LANGUAGE_IDS = _load_language_ids()


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


class _QRTreeResult(ctypes.Structure):
    _fields_ = [
        ("dict_bits", ctypes.c_char_p),
        ("dict_bits_len", ctypes.c_int32),
        ("stream_bits", ctypes.POINTER(ctypes.c_char_p)),
        ("stream_bits_len", ctypes.POINTER(ctypes.c_int32)),
        ("n_strings", ctypes.c_int32),
    ]


_lib = ctypes.CDLL(_find_library_path())

_lib.qrtree_compress_program_local.restype = ctypes.POINTER(_QRTreeResult)
_lib.qrtree_compress_program_local.argtypes = [
    ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
    ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
]

_lib.qrtree_compress_program_multilang.restype = ctypes.POINTER(_QRTreeResult)
_lib.qrtree_compress_program_multilang.argtypes = [
    ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
    ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
    ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
]

_lib.qrtree_result_free.argtypes = [ctypes.POINTER(_QRTreeResult)]


def _language_dict_path(language: str) -> str:
    return os.path.join(LANGUAGES_DIR, f"{language}.bin")


def _resolve_single_language(language: str):
    """Resolves ONE requested language, falling back to FALLBACK_LANGUAGE if missing. Returns the resolved code, or None if neither is available."""

    if language in LANGUAGE_IDS and os.path.exists(_language_dict_path(language)):
        return language

    if language not in LANGUAGE_IDS:
        print(f"note: language '{language}' not recognized (not present in LANGUAGE_IDS)")
    else:
        print(f"note: dictionary for language '{language}' not found ({_language_dict_path(language)})")

    if language != FALLBACK_LANGUAGE and FALLBACK_LANGUAGE in LANGUAGE_IDS and os.path.exists(_language_dict_path(FALLBACK_LANGUAGE)):
        print(f"note: falling back to '{FALLBACK_LANGUAGE}' for '{language}'")
        return FALLBACK_LANGUAGE

    print(f"note: no dictionary available for '{language}' (requested and fallback both missing), dropping it")
    return None


def _resolve_language_list(languages: list) -> list:
    """Resolves each requested language independently (with fallback), then deduplicates -- the fallback language never appears twice even if several requested languages collapse onto it. May return an empty list."""

    resolved = []
    for language in languages:
        r = _resolve_single_language(language)

        if r is not None and r not in resolved:
            resolved.append(r)

    if not resolved:
        print("note: no language dictionary available at all, falling back to fully local mode (no external alphabet)")

    return resolved


def _load_language_dicts(languages: list) -> list:
    """Resolves and loads dictionaries for a list of requested languages. Returns a list of (lang_id, dict_bits) tuples, possibly empty."""

    resolved = _resolve_language_list(languages)

    loaded = []
    for language in resolved:
        lang_id = LANGUAGE_IDS[language]

        with open(_language_dict_path(language), "r") as f:
            lang_bits = f.read().strip()

        loaded.append((lang_id, lang_bits))

    return loaded


def compress_program_strings(strings: list, languages: list | None = None, min_len: int = MIN_LEN_DEFAULT, max_len: int = MAX_LEN_DEFAULT, max_dict: int = MAX_DICT_DEFAULT, exh_max_depth: int = EXH_MAX_DEPTH_DEFAULT, nthreads: int = 0) -> dict:
    """Entry point called by myParser.encode(). Runs the whole unified pipeline in C"""

    if languages is None:
        languages = ["en"]

    if nthreads == 0:
        nthreads = os.cpu_count() or 1

    byte_strings = [s.encode('utf-8') for s in strings]
    n = len(byte_strings)

    arr_ptrs = (ctypes.c_char_p * n)(*byte_strings)
    arr_lens = (ctypes.c_int32 * n)(*[len(b) for b in byte_strings])

    loaded = _load_language_dicts(languages)

    if loaded:
        n_langs = len(loaded)
        lang_bufs = [bits.encode('ascii') for _, bits in loaded]

        lang_arr = (ctypes.c_char_p * n_langs)(*lang_bufs)
        lang_lens = (ctypes.c_int32 * n_langs)(*[len(b) for b in lang_bufs])
        lang_ids_arr = (ctypes.c_int32 * n_langs)(*[lid for lid, _ in loaded])

        res_ptr = _lib.qrtree_compress_program_multilang(
            arr_ptrs, arr_lens, n,
            lang_arr, lang_lens, lang_ids_arr, n_langs,
            min_len, max_len, max_dict, exh_max_depth, nthreads,
        )
    else:
        res_ptr = _lib.qrtree_compress_program_local(
            arr_ptrs, arr_lens, n,
            min_len, max_len, max_dict, exh_max_depth, nthreads,
        )

    res = res_ptr.contents

    dict_bits = res.dict_bits.decode('ascii')
    stream_bits = [res.stream_bits[i].decode('ascii') for i in range(res.n_strings)]

    _lib.qrtree_result_free(res_ptr)

    return {
        'dict_bits': dict_bits,
        'stream_bits': stream_bits,
    }