#!/usr/bin/env python3
# String decompression

import os
import sys

LANGUAGE_IDS = {
    "en": 0,
    "it": 1,
}

ID_TO_LANGUAGE = {v: k for k, v in LANGUAGE_IDS.items()}

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

def read_lang_dict(bits: str, pos: int) -> tuple:
    """Decode an external language alphabet (build_language_dict.py
    format): A bytes + (A+1) lengths -- ids 0..A-1 are real characters,
    id A is the reserved escape symbol -- followed by the D=0 placeholder
    that always closes a language dictionary (it never has fragments)."""
    
    A, pos = exp_read(bits, pos)

    alphabet = bytearray()
    for _ in range(A):
        alphabet.append(int(bits[pos:pos + 8], 2))
        pos += 8
    alphabet = bytes(alphabet)

    lengths = {}
    for i in range(A + 1):
        lengths[i] = int(bits[pos:pos + 4], 2)
        pos += 4

    lookup, max_len = canonical_lookup(lengths)

    D, pos = exp_read(bits, pos)
    if D != 0:
        raise ValueError(f"language dictionary must have D=0 (no fragments), got D={D}")

    return {
        'alphabet': alphabet,
        'lookup': lookup,
        'max_len': max_len,
        'escape_id': A,
    }, pos


def read_supplemental_alphabet(bits: str, pos: int) -> tuple:
    """Decode the local supplemental alphabet: same block shape as the
    language alphabet's real-character section, but no escape symbol of
    its own (an escape can't itself be unescapable) and usually empty
    (A=0) when the language alphabet already covers the whole program."""
    
    A, pos = exp_read(bits, pos)

    alphabet = bytearray()
    for _ in range(A):
        alphabet.append(int(bits[pos:pos + 8], 2))
        pos += 8
    alphabet = bytes(alphabet)

    lengths = {}
    for i in range(A):
        lengths[i] = int(bits[pos:pos + 4], 2)
        pos += 4

    lookup, max_len = canonical_lookup(lengths) if A > 0 else ({}, 0)

    return {
        'alphabet': alphabet,
        'lookup': lookup,
        'max_len': max_len,
    }, pos


def read_hybrid_char(bits: str, pos: int, lang_info: dict, suppl_info: dict) -> tuple:
    """Decode one character: try the external language Huffman tree
    first; if it decodes to the reserved escape id, the real byte
    follows immediately, coded with the local supplemental tree instead."""
    
    sym, pos = huffman_read_symbol(bits, pos, lang_info['lookup'], lang_info['max_len'])

    if sym == lang_info['escape_id']:
        suppl_sym, pos = huffman_read_symbol(bits, pos, suppl_info['lookup'], suppl_info['max_len'])
        return suppl_info['alphabet'][suppl_sym], pos

    return lang_info['alphabet'][sym], pos


def read_fragments_hybrid(bits: str, pos: int, lang_info: dict, suppl_info: dict) -> tuple:
    """Decode the fragments-only section (no alphabet here -- that lives
    externally): D + D entries, each length-prefixed and made of hybrid
    characters, followed by the token (fragment-index) Huffman overhead."""
    
    D, pos = exp_read(bits, pos)

    dictionary = []
    for _ in range(D):
        L, pos = exp_read(bits, pos)
        entry = bytearray()
        
        for _ in range(L):
            b, pos = read_hybrid_char(bits, pos, lang_info, suppl_info)
            entry.append(b)
        
        dictionary.append(bytes(entry))

    tok_lengths = {}
    for i in range(D):
        tok_lengths[i] = int(bits[pos:pos + 4], 2)
        pos += 4
    tok_lookup, tok_max_len = canonical_lookup(tok_lengths) if D > 0 else ({}, 0)

    return {
        'dictionary': dictionary,
        'tok_lookup': tok_lookup,
        'tok_max_len': tok_max_len,
    }, pos


def read_compressed_string_hybrid(bits: str, pos: int, lang_info: dict, suppl_info: dict, frag_info: dict) -> tuple:
    """Decode one compressed program string: symbol count + flagged
    RAW/TOK symbols, exactly like the other branches -- RAW goes through
    read_hybrid_char (external-or-escape+supplemental), TOK looks up a
    fragment by its own Huffman-coded index, same as everywhere else."""
    
    N, pos = exp_read(bits, pos)

    out = bytearray()
    for _ in range(N):
        flag = bits[pos]
        pos += 1

        if flag == '0':
            b, pos = read_hybrid_char(bits, pos, lang_info, suppl_info)
            out.append(b)
        else:
            tid, pos = huffman_read_symbol(bits, pos, frag_info['tok_lookup'], frag_info['tok_max_len'])
            out.extend(frag_info['dictionary'][tid])

    return out.decode('utf-8'), pos

def load_hybrid_dictionaries(lang_id: int, bits: str, pos: int, dictionaries_dir: str) -> tuple:
    """Everything the scanner needs after reading lang_id from the
    header: resolve the language, load dictionaries/languages/<lang>.bin,
    and parse the language alphabet + local supplemental alphabet +
    fragments that follow it in the bytecode -- all the actual
    compression/decompression logic, kept out of the scanner (which only
    does lexer/parser plumbing).

    On any failure (unknown language, missing/corrupted file, malformed
    section) logs a clear error and terminates (sys.exit(1)) instead of a
    confusing low-level traceback or, worse, a silently wrong decode.

    Returns (lang_info, suppl_info, frag_info, new_pos).
    """
    
    language = ID_TO_LANGUAGE.get(lang_id)
    if language is None:
        print(f"ERROR: unknown lang_id {lang_id} (no entry in ID_TO_LANGUAGE). "
              f"Known ids: {sorted(ID_TO_LANGUAGE)}")
        sys.exit(1)

    dict_path = os.path.join(dictionaries_dir, "languages", f"{language}.bin")

    try:
        with open(dict_path, "r") as dict_file:
            lang_bits = dict_file.read().strip()
    except OSError as e:
        print(f"ERROR: language dictionary for '{language}' (lang_id={lang_id}) not found or unreadable: {dict_path} ({e})")
        sys.exit(1)

    if not lang_bits or any(c not in "01" for c in lang_bits):
        print(f"ERROR: language dictionary for '{language}' is corrupted (not a valid '0'/'1' bitstring): {dict_path}")
        sys.exit(1)

    try:
        lang_info, lang_end_pos = read_lang_dict(lang_bits, 0)
        
        if lang_end_pos != len(lang_bits):
            raise ValueError("unexpected trailing data after parsing")

        suppl_info, pos = read_supplemental_alphabet(bits, pos)
        frag_info, pos = read_fragments_hybrid(bits, pos, lang_info, suppl_info)
    except Exception as e:
        print(f"ERROR: failed to parse hybrid dictionary section (lang_id={lang_id}, language='{language}'): {e}")
        sys.exit(1)

    return lang_info, suppl_info, frag_info, pos