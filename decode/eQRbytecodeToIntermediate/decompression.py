#!/usr/bin/env python3
# String decompression

import importlib.util
import os
import sys
import urllib.request

FETCH_TIMEOUT_SECONDS = 5

_HERE = os.path.dirname(os.path.abspath(__file__))
DICTIONARIES_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "dictionaries"))


def _load_shared_lang_module():
    path = os.path.join(DICTIONARIES_DIR, "lang_ids.py")

    if not os.path.exists(path):
        raise FileNotFoundError(f"Shared language module not found: {path}. This file is the single source of truth for LANGUAGE_IDS, LANGUAGE_URLS and the dictionary fingerprint, shared with compression.py.")

    spec = importlib.util.spec_from_file_location("qrtree_lang_ids", path)

    if spec is None or spec.loader is None:
        raise ImportError(f"Could not load module spec for {path}")

    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    return mod


_lang_module = _load_shared_lang_module()
LANGUAGE_IDS = _lang_module.LANGUAGE_IDS
LANGUAGE_URLS = _lang_module.LANGUAGE_URLS
FINGERPRINT_BITS = _lang_module.FINGERPRINT_BITS
compute_fingerprint = _lang_module.compute_fingerprint
ID_TO_LANGUAGE = {v: k for k, v in LANGUAGE_IDS.items()}


def exp_read(bits: str, pos: int, n0: int = 4) -> tuple:
    """Decode one exponential-encoded unsigned int."""

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
    """Build a (code, length) -> symbol lookup from canonical Huffman lengths."""

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
    """Decode one canonical Huffman symbol."""

    cur = 0
    for L in range(1, max_len + 1):
        cur = (cur << 1) | int(bits[pos])
        pos += 1

        if (cur, L) in lookup:
            return lookup[(cur, L)], pos

    raise ValueError("Invalid Huffman code")


def read_lang_dict(bits: str, pos: int) -> tuple:
    """Decode an external language alphabet (A bytes + A+1 lengths, last id = escape) + trailing D=0.
    The escape id is returned."""

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
    """Decode the auxiliary alphabet (chars none of the N external languages cover), no escape of its own, it is always the terminal of the chain."""

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


def read_local_alphabet(bits: str, pos: int) -> tuple:
    """Decode the full local alphabet (A bytes + A lengths)."""

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


def read_char_unified(bits: str, pos: int, mode: int, char_info, suppl_info) -> tuple:
    """Decode one character. Mode 0 (multilanguage, escape chain): tries langs[0]'s tree; if the decoded symbol is its escape, tries langs[1]; and so on; if the last language's escape triggers too, reads the auxiliary tree (which has no escape of its own).
    Mode 1 (all local): no chain, reads the single local tree directly."""

    if mode == 0:
        for lang in char_info['langs']:
            sym, pos = huffman_read_symbol(bits, pos, lang['lookup'], lang['max_len'])

            if sym != lang['escape_id']:
                return lang['alphabet'][sym], pos

        sym, pos = huffman_read_symbol(bits, pos, suppl_info['lookup'], suppl_info['max_len'])
        return suppl_info['alphabet'][sym], pos

    sym, pos = huffman_read_symbol(bits, pos, char_info['lookup'], char_info['max_len'])
    return char_info['alphabet'][sym], pos


def read_fragments_unified(bits: str, pos: int, mode: int, char_info, suppl_info) -> tuple:
    """Decode the fragments section: entry count, each length-prefixed entry, then the token Huffman overhead."""

    D, pos = exp_read(bits, pos)

    dictionary = []
    for _ in range(D):
        L, pos = exp_read(bits, pos)
        entry = bytearray()

        for _ in range(L):
            b, pos = read_char_unified(bits, pos, mode, char_info, suppl_info)
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


def read_compressed_string_unified(bits: str, pos: int, mode: int, char_info, suppl_info, frag_info: dict) -> tuple:
    """Decode one compressed program string: symbol count + flagged RAW/TOK symbols."""

    N, pos = exp_read(bits, pos)

    out = bytearray()
    for _ in range(N):
        flag = bits[pos]
        pos += 1

        if flag == '0':
            b, pos = read_char_unified(bits, pos, mode, char_info, suppl_info)
            out.append(b)
        else:
            tid, pos = huffman_read_symbol(bits, pos, frag_info['tok_lookup'], frag_info['tok_max_len'])
            out.extend(frag_info['dictionary'][tid])

    return out.decode('utf-8'), pos


def _try_fetch_language_dict(language: str, url: str):
    """Tries to fetch a language dictionary from its configured URL.
    Returns the validated bitstring content, or None on any failure (network, timeout, invalid content)."""

    try:
        with urllib.request.urlopen(url, timeout=FETCH_TIMEOUT_SECONDS) as response:
            raw = response.read()
    except OSError as e:
        print(f"note: fetch failed for '{language}' from {url}: {e}")
        return None

    try:
        lang_bits = raw.decode('ascii').strip()
    except UnicodeDecodeError:
        print(f"note: fetched content for '{language}' from {url} is not valid ASCII, discarding")
        return None

    if not lang_bits or any(c not in "01" for c in lang_bits):
        print(f"note: fetched content for '{language}' from {url} is not a valid '0'/'1' bitstring, discarding")
        return None

    print(f"note: fetched '{language}' from {url} ({len(lang_bits)} bit)")
    return lang_bits


def _load_one_language(lang_id: int, expected_fingerprint: int, dictionaries_dir: str) -> dict:
    """Loads and parses a single external language dictionary by id, checking its content fingerprint against the one embedded in the header. Tries the local cache first; only on a mismatch (or if missing) does it try a remote fetch, and only keeps a fetched copy if IT ALSO matches the expected fingerprint.
    Terminates on any failure to obtain a matching dictionary."""

    language = ID_TO_LANGUAGE.get(lang_id)
    if language is None:
        print(f"ERROR: unknown lang_id {lang_id} (no entry in ID_TO_LANGUAGE). Known ids: {sorted(ID_TO_LANGUAGE)}")
        sys.exit(1)

    dict_path = os.path.join(dictionaries_dir, f"{language}.bin")
    lang_bits = None

    if os.path.exists(dict_path):
        with open(dict_path, "r") as f:
            local_bits = f.read().strip()

        if local_bits and all(c in "01" for c in local_bits):
            local_fingerprint = compute_fingerprint(local_bits)

            if local_fingerprint == expected_fingerprint:
                print(f"note: using local dictionary for '{language}' (lang_id={lang_id}, fingerprint={local_fingerprint:#06x} matches)")
                lang_bits = local_bits
            else:
                print(f"note: local dictionary for '{language}' does not match (local={local_fingerprint:#06x}, expected={expected_fingerprint:#06x}), trying remote fetch")
        else:
            print(f"note: local dictionary for '{language}' is corrupted, trying remote fetch")
    else:
        print(f"note: no local dictionary for '{language}' (lang_id={lang_id}), trying remote fetch")

    if lang_bits is None:
        url = LANGUAGE_URLS.get(language)

        if url:
            fetched = _try_fetch_language_dict(language, url)

            if fetched is not None:
                fetched_fingerprint = compute_fingerprint(fetched)

                if fetched_fingerprint == expected_fingerprint:
                    print(f"note: fetched dictionary for '{language}' matches (fingerprint={fetched_fingerprint:#06x}), saving to local cache")

                    with open(dict_path, "w") as f:
                        f.write(fetched)

                    lang_bits = fetched
                else:
                    print(f"ERROR: fetched dictionary for '{language}' from {url} does not match this eQR code (fetched fingerprint={fetched_fingerprint:#06x}, expected={expected_fingerprint:#06x})"
                    )
                    sys.exit(1)

    if lang_bits is None:
        print(f"ERROR: no matching dictionary available for '{language}' (lang_id={lang_id}, expected fingerprint={expected_fingerprint:#06x}): local missing/mismatched and no working remote fetch")
        sys.exit(1)

    try:
        lang_info, lang_end_pos = read_lang_dict(lang_bits, 0)

        if lang_end_pos != len(lang_bits):
            raise ValueError("unexpected trailing data after parsing")
    except Exception as e:
        print(f"ERROR: failed to parse language dictionary (lang_id={lang_id}, language='{language}'): {e}")
        sys.exit(1)

    return lang_info


def load_unified_dictionaries(bits: str, pos: int, dictionaries_dir: str) -> tuple:
    """Read the 1-bit mode selector and load whatever it points to: bit '0' -> N external language dictionaries in chain order + auxiliary; bit '1' -> a full local alphabet.
    Terminates on any failure."""

    mode = int(bits[pos])
    pos += 1

    if mode == 0:
        n_langs, pos = exp_read(bits, pos)

        lang_ids = []
        lang_fingerprints = []
        for _ in range(n_langs):
            lid, pos = exp_read(bits, pos)
            fp = int(bits[pos:pos + FINGERPRINT_BITS], 2)
            pos += FINGERPRINT_BITS

            lang_ids.append(lid)
            lang_fingerprints.append(fp)

        langs = [_load_one_language(lid, fp, dictionaries_dir) for lid, fp in zip(lang_ids, lang_fingerprints)]
        char_info = {'langs': langs}

        try:
            suppl_info, pos = read_supplemental_alphabet(bits, pos)
            frag_info, pos = read_fragments_unified(bits, pos, 0, char_info, suppl_info)
        except Exception as e:
            print(f"ERROR: failed to parse multilang dictionary section (lang_ids={lang_ids}): {e}")
            sys.exit(1)

        return 0, char_info, suppl_info, frag_info, pos

    else:
        try:
            local_info, pos = read_local_alphabet(bits, pos)
            frag_info, pos = read_fragments_unified(bits, pos, 1, local_info, None)
        except Exception as e:
            print(f"ERROR: failed to parse local dictionary section: {e}")
            sys.exit(1)

        return 1, local_info, None, frag_info, pos