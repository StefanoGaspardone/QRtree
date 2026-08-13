#!/usr/bin/env python3
# String decompression

import os
import sys


def exp_read(bits: str, pos: int, n0: int = 4) -> tuple:
    """Decode one exponential-encoded unsigned int starting at pos."""
    
    n = n0
    ext = 0
    total = 0

    while True:
        max_val = (1 << n) - 1
        chunk = bits[pos:pos + n]
        pos += n
        v = int(chunk, 2)

        if v < max_val:
            return total + v, pos

        total += max_val
        if ext > 0:
            n *= 2
        ext += 1


def canonical_lookup(lengths: dict) -> tuple:
    """Invert canonical_codes: map (code, length) back to the original symbol."""
    
    syms = sorted(lengths.keys(), key=lambda s: (lengths[s], s))

    lookup = {}
    code = 0
    prev = 0
    max_len = 0

    for s in syms:
        L = lengths[s]
        
        if L > prev:
            code <<= (L - prev)
        
        lookup[(code, L)] = s
        code += 1
        prev = L
        max_len = max(max_len, L)

    return lookup, max_len


def huffman_read_symbol(bits: str, pos: int, lookup: dict, max_len: int) -> tuple:
    """Decode one canonical Huffman symbol starting at pos."""
    
    cur = 0
    for L in range(1, max_len + 1):
        cur = (cur << 1) | int(bits[pos])
        pos += 1
        
        if (cur, L) in lookup:
            return lookup[(cur, L)], pos
    
    raise ValueError("Invalid Huffman code")


def read_dict(bits: str, pos: int) -> tuple:
    """Decode the local dictionary body (alphabet + fragments) starting at pos."""
    
    A, pos = exp_read(bits, pos)

    alphabet = bytearray()
    for _ in range(A):
        alphabet.append(int(bits[pos:pos + 8], 2))
        pos += 8
    alphabet = bytes(alphabet)

    char_lengths = {}
    for i in range(A):
        char_lengths[i] = int(bits[pos:pos + 4], 2)
        pos += 4
    char_lookup, char_max_len = canonical_lookup(char_lengths)

    D, pos = exp_read(bits, pos)

    dictionary = []
    for _ in range(D):
        L, pos = exp_read(bits, pos)
        entry = bytearray()
        
        for _ in range(L):
            cid, pos = huffman_read_symbol(bits, pos, char_lookup, char_max_len)
            entry.append(alphabet[cid])
        
        dictionary.append(bytes(entry))

    tok_lengths = {}
    for i in range(D):
        tok_lengths[i] = int(bits[pos:pos + 4], 2)
        pos += 4
    tok_lookup, tok_max_len = canonical_lookup(tok_lengths) if D > 0 else ({}, 0)

    return {
        'alphabet': alphabet,
        'dictionary': dictionary,
        'char_lookup': char_lookup,
        'char_max_len': char_max_len,
        'tok_lookup': tok_lookup,
        'tok_max_len': tok_max_len,
    }, pos


def read_compressed_string(bits: str, pos: int, dict_info: dict) -> tuple:
    """Decode one compressed string starting at pos."""
    
    alphabet = dict_info['alphabet']
    dictionary = dict_info['dictionary']

    N, pos = exp_read(bits, pos)

    out = bytearray()
    for _ in range(N):
        flag = bits[pos]
        pos += 1

        if flag == '0':
            cid, pos = huffman_read_symbol(bits, pos, dict_info['char_lookup'], dict_info['char_max_len'])
            out.append(alphabet[cid])
        else:
            tid, pos = huffman_read_symbol(bits, pos, dict_info['tok_lookup'], dict_info['tok_max_len'])
            out.extend(dictionary[tid])

    return out.decode('utf-8'), pos


def load_external_dict(dict_id: int, dictionaries_dir: str) -> dict:
    """Everything the scanner needs after reading dict_id from the header:
    load dictionaries_dir/<dict_id>.bin and parse it -- all the actual
    file I/O and error handling, kept out of the scanner (which only does
    lexer/parser plumbing).

    On any failure (missing/corrupted file, malformed content) logs a
    clear error and terminates (sys.exit(1)) instead of a confusing
    low-level traceback or, worse, a silently wrong decode.
    """
    
    dict_path = os.path.join(dictionaries_dir, f"{dict_id}.bin")

    try:
        with open(dict_path, "r") as dict_file:
            ext_bits = dict_file.read().strip()
    except OSError as e:
        print(f"ERROR: external dictionary file for id {dict_id} not found or unreadable: {dict_path} ({e})")
        sys.exit(1)

    if not ext_bits or any(c not in "01" for c in ext_bits):
        print(f"ERROR: external dictionary file for id {dict_id} is corrupted (not a valid '0'/'1' bitstring): {dict_path}")
        sys.exit(1)

    try:
        dict_info, end_pos = read_dict(ext_bits, 0)
        
        if end_pos != len(ext_bits):
            raise ValueError("unexpected trailing data after parsing")
    except Exception as e:
        print(f"ERROR: external dictionary file for id {dict_id} is corrupted (failed to parse): {dict_path} ({e})")
        sys.exit(1)

    return dict_info