#!/usr/bin/env python3
# String compression

from collections import defaultdict
import heapq
import math

RAW = 0
TOK = 1


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


def reference_encoding_size_bits(x: int) -> int:
    """Bit length of x's exponential encoding."""
    
    return len(reference_encoding(x))


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
    """Bitstring for one symbol's canonical Huffman code."""
    
    val, length = codes[sym_id]
    return format(val, f'0{length}b')


def huffman_overhead_bits(freqs: dict, count: int) -> str:
    """Length table (4 bits/symbol) written to the header for decoding."""
    
    lens = huffman_len_lengths(freqs, count) if count > 0 else {}
    return "".join(format(lens.get(i, 1), '04b') for i in range(count))


def compute_char_bit_lengths(alphabet: bytes, char_freqs: dict) -> dict:
    """Map each alphabet byte to its Huffman code length."""
    
    lengths_by_id = huffman_len_lengths(char_freqs, len(alphabet))
    return {alphabet[i]: lengths_by_id[i] for i in range(len(alphabet))}


def build_alphabet(byte_strings: list) -> tuple:
    """Collect the distinct bytes used across all strings, with frequencies."""
    
    freq = defaultdict(int)

    for bs in byte_strings:
        for bv in bs:
            freq[bv] += 1

    alph = bytes(sorted(freq))
    inv = {alph[i]: i for i in range(len(alph))}

    return alph, inv, needed_bits(len(alph)), freq


def initial_sequences(byte_strings: list) -> list:
    """Turn raw byte strings into (RAW, byte) symbol sequences."""
    
    return [[(RAW, x) for x in bs] for bs in byte_strings]


def find_candidates(seqs: list, min_len: int = 2, max_len: int = 32) -> dict:
    """Find repeated raw substrings (length >= min_len) across all sequences."""
    
    counts = defaultdict(int)

    for seq in seqs:
        n = len(seq)
        
        for i in range(n):
            if seq[i][0] != RAW:
                continue
            
            acc = []
            
            for j in range(i, min(n, i + max_len)):
                if seq[j][0] != RAW:
                    break
                
                acc.append(seq[j][1])
                
                if len(acc) >= min_len:
                    counts[bytes(acc)] += 1

    return {k: v for k, v in counts.items() if v >= 2}


def count_non_overlapping(seq: list, pat_bytes: bytes) -> int:
    """Count non-overlapping occurrences of a pattern in one sequence."""
    
    pat = [(RAW, b) for b in pat_bytes]
    m = len(pat)
    i = 0
    c = 0

    while i <= len(seq) - m:
        if seq[i : i+m] == pat:
            c += 1
            i += m
        else:
            i += 1

    return c


def total_non_overlapping(seqs: list, pat_bytes: bytes) -> int:
    """Total non-overlapping occurrences of a pattern across all sequences."""
    
    return sum(count_non_overlapping(seq, pat_bytes) for seq in seqs)


def replace_non_overlapping(seqs: list, pat_bytes: bytes, token_id: int) -> list:
    """Replace non-overlapping occurrences of a pattern with a dictionary token."""
    
    pat = [(RAW, b) for b in pat_bytes]
    m = len(pat)
    token = (TOK, token_id)
    out_all = []

    for seq in seqs:
        out = []
        i = 0
        
        while i < len(seq):
            if i <= len(seq) - m and seq[i:i + m] == pat:
                out.append(token)
                i += m
            else:
                out.append(seq[i])
                i += 1
        
        out_all.append(out)

    return out_all


def count_tok_freqs(seqs: list) -> dict:
    """Frequency of each dictionary token across all sequences."""
    
    tok_freqs = defaultdict(int)
    
    for seq in seqs:
        for typ, val in seq:
            if typ == TOK:
                tok_freqs[val] += 1
    
    return tok_freqs


def score_dictionary_bits(dictionary: list, seqs: list, char_bit_lengths: dict) -> int:
    """Total encoded size (bits) of a candidate dictionary + compressed sequences."""
    
    D = len(dictionary)
    tok_bits = huffman_len_lengths(count_tok_freqs(seqs), D)

    dict_bits = D * 4  # length-table overhead for dictionary entries
    for entry in dictionary:
        entry_header_bits = reference_encoding_size_bits(len(entry))
        entry_body_bits = sum(char_bit_lengths[b] for b in entry)
        dict_bits += entry_header_bits + entry_body_bits

    stream_bits = 0
    for seq in seqs:
        seq_header_bits = reference_encoding_size_bits(len(seq))
        seq_body_bits = 0
        
        for typ, val in seq:
            seq_body_bits += 1
            seq_body_bits += char_bit_lengths[val] if typ == RAW else tok_bits[val]
        
        stream_bits += seq_header_bits + seq_body_bits

    return dict_bits + stream_bits


def token_bits_for_candidate(tok_freqs: dict, d: int, occ: int) -> int:
    """Estimated Huffman code length for a not-yet-added candidate token."""
    
    combined = dict(tok_freqs)
    combined[d] = occ
    
    return huffman_len_lengths(combined, d + 1)[d]


def scoring_function(pat_bytes: bytes, occ: int, char_bit_lengths: dict, token_bits_after: int) -> float:
    """Net bit savings from adding a candidate pattern to the dictionary."""
    
    L = len(pat_bytes)
    pat_bits = sum(char_bit_lengths[b] for b in pat_bytes)

    old_cost = occ * (L + pat_bits)
    new_cost = occ * (1 + token_bits_after)
    dict_cost = reference_encoding_size_bits(L) + pat_bits

    return old_cost - new_cost - dict_cost


def greedy_build(byte_strings, char_bit_lengths, min_len = 2, max_len = 32, max_dict = 1023, init_dict = None, init_seqs = None) -> tuple:
    """Greedily grow the dictionary by repeatedly adding the best-scoring pattern."""
    
    seqs = init_seqs[:] if init_seqs is not None else initial_sequences(byte_strings)
    dictionary = list(init_dict) if init_dict is not None else []

    current_bits = score_dictionary_bits(dictionary, seqs, char_bit_lengths)

    while len(dictionary) < max_dict:
        D = len(dictionary)
        tok_freqs = count_tok_freqs(seqs)
        candidates = find_candidates(seqs, min_len, max_len)

        best = None
        best_gain = 0

        for pat in candidates:
            occ = total_non_overlapping(seqs, pat)
            
            if occ < 2:
                continue
            
            token_bits_after = token_bits_for_candidate(tok_freqs, D, occ)
            gain = scoring_function(pat, occ, char_bit_lengths, token_bits_after)
            
            if gain > best_gain:
                best_gain = gain
                best = pat

        if best is None or best_gain <= 0:
            break

        trial_dict = dictionary + [best]
        trial_seqs = replace_non_overlapping(seqs, best, D)
        trial_bits = score_dictionary_bits(trial_dict, trial_seqs, char_bit_lengths)

        if trial_bits >= current_bits:
            break

        dictionary = trial_dict
        seqs = trial_seqs
        current_bits = trial_bits

    return dictionary, seqs


def build_dictionary(byte_strings, char_bit_lengths, min_len = 2, max_len = 32, max_dict = 1023, max_depth = None) -> tuple:
    """Build the dictionary via greedy search, or DFS branch & bound if max_depth != 0."""
    
    if max_depth == 0:
        return greedy_build(byte_strings, char_bit_lengths, min_len, max_len, max_dict)

    init_seqs = initial_sequences(byte_strings)
    memo = {}

    def state_key(dct, sqs, depth):
        return (depth, tuple(dct), tuple(tuple(seq) for seq in sqs))

    def compute_ub_gain(candidates, sqs):
        total = 0.0
        
        for pat in candidates:
            occ = total_non_overlapping(sqs, pat)
            gain = scoring_function(pat, occ, char_bit_lengths, 1)
            
            if gain > 0:
                total += gain
        
        return total

    def dfs(dct, sqs, current_bits, depth):
        key = state_key(dct, sqs, depth)
        
        if key in memo:
            return memo[key]

        base_dct, base_sqs = greedy_build(byte_strings, char_bit_lengths, min_len, max_len, max_dict, list(dct), list(sqs))
        base_bits = score_dictionary_bits(base_dct, base_sqs, char_bit_lengths)
        best_local = (base_bits, list(base_dct), base_sqs)

        if len(dct) >= max_dict:
            memo[key] = best_local
            return best_local

        if max_depth is not None and depth >= max_depth:
            memo[key] = best_local
            return best_local

        candidates = find_candidates(sqs, min_len, max_len)
        if not candidates:
            memo[key] = best_local
            return best_local

        ub_gain_bits = compute_ub_gain(candidates, sqs)
        if current_bits - ub_gain_bits >= base_bits:
            memo[key] = best_local
            return best_local

        D = len(dct)
        tok_freqs = count_tok_freqs(sqs)
        scored = []

        for pat in candidates:
            occ = total_non_overlapping(sqs, pat)
            
            if occ < 2:
                continue
            
            token_bits_after = token_bits_for_candidate(tok_freqs, D, occ)
            gain = scoring_function(pat, occ, char_bit_lengths, token_bits_after)
            
            if gain > 0:
                scored.append((gain, pat))

        if not scored:
            memo[key] = best_local
            return best_local

        scored.sort(key=lambda x: x[0], reverse=True)

        for _, pat in scored:
            new_dct = list(dct) + [pat]
            new_sqs = replace_non_overlapping(sqs, pat, D)
            new_bits = score_dictionary_bits(new_dct, new_sqs, char_bit_lengths)

            cand_bits, cand_dct, cand_sqs = dfs(new_dct, new_sqs, new_bits, depth + 1)

            if cand_bits < best_local[0]:
                best_local = (cand_bits, cand_dct, cand_sqs)

        memo[key] = best_local
        return best_local

    init_bits = score_dictionary_bits([], init_seqs, char_bit_lengths)
    _best_bits, best_dict, best_seqs = dfs([], init_seqs, init_bits, 0)

    return best_dict, best_seqs


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


def compress_program_strings(strings: list, min_len: int = 2, max_len: int = 32, max_dict: int = 1023, exh_max_depth: int = 0) -> dict:
    """Build one shared dictionary for all given strings and tokenize each of them."""
    
    byte_strings = [s.encode("utf-8") for s in strings]

    alphabet, byte_to_id, _char_bits, raw_freq = build_alphabet(byte_strings)
    char_freqs = {byte_to_id[bv]: cnt for bv, cnt in raw_freq.items()}
    char_bit_lengths = compute_char_bit_lengths(alphabet, char_freqs)

    max_d = None if exh_max_depth < 0 else exh_max_depth
    dictionary, seqs = build_dictionary(byte_strings, char_bit_lengths, min_len, max_len, max_dict, max_d)

    tok_freqs = count_tok_freqs(seqs)

    tok_lengths_by_id = huffman_len_lengths(tok_freqs, len(dictionary))
    tok_codes = canonical_codes(tok_lengths_by_id)

    char_lengths_by_id = huffman_len_lengths(char_freqs, len(alphabet))
    char_codes = canonical_codes(char_lengths_by_id)

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