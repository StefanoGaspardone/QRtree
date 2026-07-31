#!/usr/bin/env python3
# String compression

import ctypes
import os
import platform
from collections import defaultdict
import heapq
import math

RAW = 0
TOK = 1

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


class _QBuf(ctypes.Structure):
    _fields_ = [("data", ctypes.POINTER(ctypes.c_uint8)), ("len", ctypes.c_int32)]


class _QSym(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint8), ("val", ctypes.c_int32)]


class _QSeq(ctypes.Structure):
    _fields_ = [("items", ctypes.POINTER(_QSym)), ("len", ctypes.c_int32)]


class _QRTreeBuildResult(ctypes.Structure):
    _fields_ = [
        ("alphabet", ctypes.c_uint8 * 256),
        ("A", ctypes.c_int32),
        ("byte_to_id", ctypes.c_int32 * 256),
        ("dict_entries", ctypes.POINTER(_QBuf)),
        ("dict_count", ctypes.c_int32),
        ("seqs", ctypes.POINTER(_QSeq)),
        ("seqs_count", ctypes.c_int32),
    ]


_lib = ctypes.CDLL(_find_library_path())

_lib.qrtree_build.restype = ctypes.POINTER(_QRTreeBuildResult)
_lib.qrtree_build.argtypes = [
    ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
    ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
]
_lib.qrtree_free_result.argtypes = [ctypes.POINTER(_QRTreeBuildResult)]


def _c_build_dictionary(strings, min_len, max_len, max_dict, nthreads):
    """Call into C for alphabet construction + dictionary search (the
    expensive part), always huffman-len encoding, DFS depth EXH_MAX_DEPTH.
    Returns (alphabet, byte_to_id, dictionary, seqs)."""
    
    byte_strings = [s.encode('utf-8') for s in strings]
    n = len(byte_strings)

    arr_ptrs = (ctypes.c_char_p * n)(*byte_strings)
    arr_lens = (ctypes.c_int32 * n)(*[len(b) for b in byte_strings])

    res_ptr = _lib.qrtree_build(arr_ptrs, arr_lens, n, ENC_HUFF_LEN, min_len, max_len, max_dict, EXH_MAX_DEPTH, nthreads)
    res = res_ptr.contents

    alphabet = bytes(res.alphabet[:res.A])
    byte_to_id = {i: res.byte_to_id[i] for i in range(256) if res.byte_to_id[i] >= 0}

    dictionary = []
    for i in range(res.dict_count):
        e = res.dict_entries[i]
        dictionary.append(bytes(e.data[:e.len]))

    seqs = []
    for i in range(res.seqs_count):
        s = res.seqs[i]
        seqs.append([(s.items[k].type, s.items[k].val) for k in range(s.len)])

    _lib.qrtree_free_result(res_ptr)

    return alphabet, byte_to_id, dictionary, seqs


def needed_bits(n: int) -> int:
    """Minimum number of bits needed to represent n distinct values."""
    
    return 1 if n <= 1 else math.ceil(math.log2(n))


def _exponential_ones_value(ones: int) -> int:
    """Recursive helper for the exponential encoding's cumulative offset."""
    
    if ones == 0:
        return 0
    
    if ones == 4:
        return 2 ** ones - 1
    
    return _exponential_ones_value(ones // 2) + 2 ** (ones // 2) - 1


def reference_encoding(value: int) -> str:
    """Encode a non-negative int using QRscript's exponential encoding."""
    
    if not (isinstance(value, int) and value >= 0):
        raise Exception(f"The value {value} is not a valid non-negative integer")

    length = 4

    while True:
        ones = 0 if length == 4 else length // 2
        max_value = 2 ** (length - ones) - 1
        cur_value = value - _exponential_ones_value(ones)

        if cur_value < max_value:
            return "1" * ones + format(cur_value, f'0{4 if length == 4 else length // 2}b')

        length = length * 2


def huffman_lengths(freq_map: dict) -> dict:
    """Compute optimal Huffman code lengths for a symbol frequency map."""
    
    if not freq_map:
        return {}

    if len(freq_map) == 1:
        return {next(iter(freq_map)): 1}

    heap = [[f, i, [s]] for i, (s, f) in enumerate(sorted(freq_map.items()))]
    heapq.heapify(heap)

    lengths = dict.fromkeys(freq_map, 0)
    tie = len(heap)

    while len(heap) > 1:
        f1, _, s1 = heapq.heappop(heap)
        f2, _, s2 = heapq.heappop(heap)

        for s in s1:
            lengths[s] += 1
        for s in s2:
            lengths[s] += 1

        heapq.heappush(heap, [f1 + f2, tie, s1 + s2])
        tie += 1

    return lengths


def canonical_codes(lengths: dict) -> dict:
    """Build canonical Huffman codes (symbol -> (code, length)) from lengths."""
    
    syms = sorted(lengths.keys(), key=lambda s: (lengths[s], s))

    codes = {}
    code = 0
    prev = 0

    for s in syms:
        L = lengths[s]

        if L > prev:
            code <<= (L - prev)

        codes[s] = (code, L)
        code += 1
        prev = L

    return codes


def huffman_len_lengths(freqs: dict, count: int) -> dict:
    """Huffman lengths capped at 15 bits, so they fit in a 4-bit header field."""
    
    lengths = huffman_lengths({i: freqs.get(i, 1) for i in range(count)})
    if not lengths or max(lengths.values(), default=0) <= 15:
        return lengths

    new_lengths = {sym: min(L, 15) for sym, L in lengths.items()}
    target = 1 << 15

    while True:
        kraft_sum = sum(1 << (15 - L) for L in new_lengths.values())
        
        if kraft_sum <= target:
            break
        
        candidates = [sym for sym, L in new_lengths.items() if L < 15]
        best_sym = max(candidates, key=lambda s: (new_lengths[s], s))
        new_lengths[best_sym] += 1

    return new_lengths


def huffman_symbol_bits(sym_id: int, codes: dict) -> str:
    val, length = codes[sym_id]
    return format(val, f'0{length}b')


def huffman_overhead_bits(freqs: dict, count: int) -> str:
    """Length table (4 bits/symbol) written to the header for decoding."""
    
    lens = huffman_len_lengths(freqs, count) if count > 0 else {}
    return "".join(format(lens.get(i, 1), '04b') for i in range(count))


def count_tok_freqs(seqs: list) -> dict:
    """Frequency of each dictionary token across all sequences."""
    
    tok_freqs = defaultdict(int)
    for seq in seqs:
        for typ, val in seq:
            if typ == TOK:
                tok_freqs[val] += 1
    
    return tok_freqs


def dict_bitstring(dictionary: list, alphabet: bytes, char_freqs: dict, tok_freqs: dict, byte_to_id: dict) -> str:
    """Serialize the local dictionary body (alphabet + fragments), no command opcode."""
    
    out = reference_encoding(len(alphabet))
    for b in alphabet:
        out += format(b, '08b')
    
    out += huffman_overhead_bits(char_freqs, len(alphabet))

    char_lengths_by_id = huffman_len_lengths(char_freqs, len(alphabet))
    char_codes = canonical_codes(char_lengths_by_id)

    out += reference_encoding(len(dictionary))
    for entry in dictionary:
        out += reference_encoding(len(entry))
        
        for b in entry:
            out += huffman_symbol_bits(byte_to_id[b], char_codes)
    
    out += huffman_overhead_bits(tok_freqs, len(dictionary))

    return out


def compressed_string_bitstring(seq: list, byte_to_id: dict, char_codes: dict, tok_codes: dict) -> str:
    """Serialize one tokenized string: symbol count + flagged RAW/TOK symbols."""
    
    out = reference_encoding(len(seq))

    for typ, val in seq:
        if typ == RAW:
            out += "0" + huffman_symbol_bits(byte_to_id[val], char_codes)
        else:
            out += "1" + huffman_symbol_bits(val, tok_codes)

    return out


def compress_program_strings(strings: list, min_len: int = MIN_LEN_DEFAULT, max_len: int = MAX_LEN_DEFAULT, max_dict: int = MAX_DICT_DEFAULT, nthreads: int = 0) -> dict:
    """Entry point: dictionary search in C, then Python codec + serialization."""
    
    if nthreads == 0:
        nthreads = os.cpu_count() or 1

    alphabet, byte_to_id, dictionary, seqs = _c_build_dictionary(strings, min_len, max_len, max_dict, nthreads)

    byte_strings = [s.encode('utf-8') for s in strings]
    char_freq_by_byte = defaultdict(int)
    for bs in byte_strings:
        for b in bs:
            char_freq_by_byte[b] += 1
    char_freqs = {byte_to_id[b]: cnt for b, cnt in char_freq_by_byte.items() if b in byte_to_id}

    tok_freqs = count_tok_freqs(seqs)

    char_lengths_by_id = huffman_len_lengths(char_freqs, len(alphabet))
    char_codes = canonical_codes(char_lengths_by_id)

    tok_lengths_by_id = huffman_len_lengths(tok_freqs, len(dictionary))
    tok_codes = canonical_codes(tok_lengths_by_id)

    dict_bits = dict_bitstring(dictionary, alphabet, char_freqs, tok_freqs, byte_to_id)

    return {
        'dict_bits': dict_bits,
        'seqs': seqs,
        'byte_to_id': byte_to_id,
        'char_codes': char_codes,
        'tok_codes': tok_codes,
        'dictionary': dictionary,
        'alphabet': alphabet,
    }