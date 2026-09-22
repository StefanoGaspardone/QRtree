#!/usr/bin/env python3

import argparse
import heapq
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS_DIR = os.path.join(_HERE, "corpus")
OUTPUT_DIR = os.path.join(_HERE, "..")


def _exponential_ones_value(ones: int) -> int:
    if ones == 0:
        return 0
    if ones == 4:
        return 2 ** ones - 1
    return _exponential_ones_value(ones // 2) + 2 ** (ones // 2) - 1


def reference_encoding(value: int) -> str:
    length = 4
    while True:
        ones = 0 if length == 4 else length // 2
        max_value = 2 ** (length - ones) - 1
        cur_value = value - _exponential_ones_value(ones)
        if cur_value < max_value:
            return "1" * ones + format(cur_value, f"0{4 if length == 4 else length // 2}b")
        length *= 2


def huffman_lengths(freq_map: dict) -> dict:
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


def huffman_len_lengths(freqs: dict, count: int) -> dict:
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


ESCAPE_FREQ_METHOD = "witten-bell"


def build_dict_bits(corpus_bytes: bytes) -> tuple:
    freq_by_byte = {}
    for b in corpus_bytes:
        freq_by_byte[b] = freq_by_byte.get(b, 0) + 1

    alphabet = sorted(freq_by_byte.keys())
    A = len(alphabet)

    freq_by_id = {i: freq_by_byte[b] for i, b in enumerate(alphabet)}

    escape_id = A
    freq_with_escape = dict(freq_by_id)
    freq_with_escape[escape_id] = A if A > 0 else 1

    char_lengths = huffman_len_lengths(freq_with_escape, A + 1)
    escape_length = char_lengths[escape_id]

    bits = reference_encoding(A)
    for b in alphabet:
        bits += format(b, "08b")
    
    for i in range(A + 1):
        bits += format(char_lengths[i], "04b")

    bits += reference_encoding(0)

    return bits, escape_length


def build_language_dict(lang_code: str):
    corpus_path = os.path.join(CORPUS_DIR, f"{lang_code}.txt")

    if not os.path.exists(corpus_path):
        print(f"ERRORE: corpus non trovato per la lingua '{lang_code}': {corpus_path}")
        print("Metti un file di testo rappresentativo in quel percorso e riprova.")
        sys.exit(1)

    with open(corpus_path, "r", encoding="utf-8") as f:
        corpus = f.read()

    corpus_bytes = corpus.encode("utf-8")

    dict_bits, escape_length = build_dict_bits(corpus_bytes)

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    output_path = os.path.join(OUTPUT_DIR, f"{lang_code}.bin")

    with open(output_path, "w") as f:
        f.write(dict_bits)

    corpus_chars = set(corpus_bytes)
    print(f"lingua={lang_code}: corpus={len(corpus_bytes)} byte UTF-8 ({len(corpus_chars)} valori distinti)")
    print(f"simbolo escape (id={len(corpus_chars)}): lunghezza codice = {escape_length} bit (metodo: {ESCAPE_FREQ_METHOD})")
    print(f"scritto {output_path}: {len(dict_bits)} bit ({len(dict_bits) // 8 + 1} byte)")


def main():
    ap = argparse.ArgumentParser(description="Costruisce il dizionario-alfabeto ottimizzato per una lingua (huffman-len), script autonomo")
    ap.add_argument("lang", help="codice lingua (es. 'en'), deve avere un corpus in corpora/<lang>.txt")

    args = ap.parse_args()
    build_language_dict(args.lang)


if __name__ == "__main__":
    main()