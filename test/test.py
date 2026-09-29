import subprocess
import os
import sys
import time
from datetime import datetime

TEST_CASES = [
    ('wifi.txt', [['en'], ['it'], ['en', 'it'], ['xx']]),
    ('switch.txt', [['en'], ['it'], ['en', 'it'], ['xx']]),
    ('defibrillator.txt', [['en'], ['it'], ['en', 'it'], ['xx']]),
    ('mountain.txt', [['en'], ['it'], ['en', 'it'], ['xx']]),
    ('colonnina.txt', [['en'], ['it'], ['en', 'it'], ['xx']]),
    ('colonnina-ch.txt', [['ch'], ['it'], ['ch', 'it']]),
    ('colonnina-fr.txt', [['fr'], ['en']]),
    ('colonnina-de.txt', [['de'], ['en']]),
]

MAX_DEPTHS = [0, 1, 2] if sys.platform.startswith('linux') else [0, 1]

PYTHON = sys.executable

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TEST_DIR)
QRTREE_SCRIPT = os.path.join(PROJECT_ROOT, 'QRtree.py')
TEST_SUBDIR_NAME = os.path.basename(TEST_DIR)

LOG_PATH = os.path.join(TEST_DIR, 'log.txt')

ID_TO_LANGUAGE = {0: 'en', 1: 'it', 2: 'ch', 3: 'fr', 4: 'de'}

FINGERPRINT_BITS = 16

DICT_HEADER_CMD_BITS = 3


def cleanup_stale(basename_abs):
    for ext in ('.qr', '.bin', '.png'):
        path = basename_abs + ext

        if os.path.exists(path):
            os.remove(path)


def exp_read(bits, pos, n0 = 4):
    n = n0
    ext = 0
    total = 0

    while True:
        max_val = (1 << n) - 1
        v = int(bits[pos:pos + n], 2)
        pos += n

        if v < max_val:
            return total + v, pos

        total += max_val

        if ext > 0:
            n *= 2

        ext += 1


def extract_header_summary(bin_path):
    if not os.path.exists(bin_path):
        return None

    with open(bin_path, 'r') as f:
        bits = f.read()

    if not bits:
        return None

    pos = DICT_HEADER_CMD_BITS

    mode = int(bits[pos])
    pos += 1

    if mode == 1:
        A_local, pos = exp_read(bits, pos)
        pos += A_local * 8   # byte dell'alfabeto
        pos += A_local * 4   # lunghezze a 4 bit

        D, pos = exp_read(bits, pos)

        return {
            'chain_order': 'locale',
            'aux_count': None,
            'local_alphabet_count': A_local,
            'fragments_count': D,
        }

    n_langs, pos = exp_read(bits, pos)
    lang_ids = []
    for _ in range(n_langs):
        lid, pos = exp_read(bits, pos)
        pos += FINGERPRINT_BITS
        lang_ids.append(lid)

    names = [ID_TO_LANGUAGE.get(lid, f'id{lid}') for lid in lang_ids]
    chain_order = '>'.join(names) if names else '(nessuna)'

    A_suppl, pos = exp_read(bits, pos)
    pos += A_suppl * 8
    pos += A_suppl * 4

    D, pos = exp_read(bits, pos)

    return {
        'chain_order': chain_order,
        'aux_count': A_suppl,
        'local_alphabet_count': None,
        'fragments_count': D,
    }


def run_one(input_file, languages, max_depth):
    basename_abs = os.path.join(TEST_DIR, os.path.splitext(input_file)[0])
    cleanup_stale(basename_abs)

    input_arg = os.path.join(TEST_SUBDIR_NAME, input_file)
    lang_arg = ','.join(languages)
    cmd = [PYTHON, QRTREE_SCRIPT, 'encode', input_arg, '-l', lang_arg, '-m', str(max_depth), '--no-cleanup']

    t0 = time.time()
    proc = subprocess.run(cmd, capture_output = True, text = True, cwd = PROJECT_ROOT, timeout = None)
    elapsed = time.time() - t0

    bin_path = basename_abs + '.bin'
    bin_size = os.path.getsize(bin_path) if os.path.exists(bin_path) else None
    header_info = extract_header_summary(bin_path) or {}

    png_path = basename_abs + '.png'
    png_size = os.path.getsize(png_path) if os.path.exists(png_path) else None

    cleanup_stale(basename_abs)

    return {
        'input_file': input_file,
        'languages': lang_arg,
        'max_depth': max_depth,
        'returncode': proc.returncode,
        'bin_size_bytes': bin_size,
        'png_size_bytes': png_size,
        'chain_order': header_info.get('chain_order'),
        'aux_count': header_info.get('aux_count'),
        'local_alphabet_count': header_info.get('local_alphabet_count'),
        'fragments_count': header_info.get('fragments_count'),
        'elapsed_s': round(elapsed, 2),
        'stdout': proc.stdout.strip(),
        'stderr': proc.stderr.strip(),
    }


def main():
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')

    results = []
    for input_file, combos in TEST_CASES:
        for languages in combos:
            for max_depth in MAX_DEPTHS:
                lang_label = '+'.join(languages)
                print(f'--- {input_file} | lingue = {lang_label} | max_depth = {max_depth} ---')

                result = run_one(input_file, languages, max_depth)
                results.append(result)

                returncode = result['returncode']
                status = 'OK' if returncode == 0 else f'ERRORE (exit {returncode})'
                bin_size = result['bin_size_bytes']
                png_size = result['png_size_bytes']
                bin_str = f'{bin_size} byte' if bin_size is not None else 'N/D'
                png_str = f'{png_size} byte' if png_size is not None else 'N/D'
                elapsed_s = result['elapsed_s']
                chain_order = result['chain_order']
                aux_count = result['aux_count']
                frag_count = result['fragments_count']

                print(f'  {status} | .bin = {bin_str} | .png = {png_str} | catena = {chain_order} | ausiliario = {aux_count} | frammenti = {frag_count} | {elapsed_s}s')

    with open(LOG_PATH, 'w', encoding = 'utf-8') as f:
        f.write(f'QRtree encode sweep -- {timestamp}\n')
        f.write('=' * 70 + '\n\n')

        header = '{:<25}{:<10}{:<10}{:<10}{:<12}{:<12}{:<10}{:<20}{:<12}{:<12}'.format(
            'File', 'Lingue', 'Max depth', 'Esito', 'Bin (byte)', 'Png (byte)', 'Tempo (s)', 'Ordine catena', 'Ausiliario', 'Frammenti'
        )
        f.write(header + '\n')
        f.write('-' * 135 + '\n')

        for r in results:
            esito = 'OK' if r['returncode'] == 0 else 'ERRORE'
            bin_str = str(r['bin_size_bytes']) if r['bin_size_bytes'] is not None else 'N/D'
            png_str = str(r['png_size_bytes']) if r['png_size_bytes'] is not None else 'N/D'
            chain_order = r['chain_order'] or 'N/D'
            aux_str = str(r['aux_count']) if r['aux_count'] is not None else '-'
            frag_str = str(r['fragments_count']) if r['fragments_count'] is not None else 'N/D'

            row = '{:<25}{:<10}{:<10}{:<10}{:<12}{:<12}{:<10}{:<20}{:<12}{:<12}'.format(
                r['input_file'], r['languages'], r['max_depth'], esito, bin_str, png_str, r['elapsed_s'], chain_order, aux_str, frag_str
            )
            f.write(row + '\n')

        f.write('\n\n' + '=' * 70 + '\n')
        f.write('DETTAGLIO PER RUN\n')
        f.write('=' * 70 + '\n\n')

        for r in results:
            input_file = r['input_file']
            languages = r['languages']
            max_depth = r['max_depth']

            f.write(f'### {input_file} | lingue = {languages} | max_depth = {max_depth} ###\n')
            f.write(f'exit code: {r["returncode"]}\n')
            f.write(f'.bin size: {r["bin_size_bytes"]} byte\n')
            f.write(f'.png size: {r["png_size_bytes"]} byte\n')
            f.write(f'ordine catena: {r["chain_order"]}\n')

            if r['aux_count'] is not None:
                f.write(f'dizionario ausiliario: {r["aux_count"]} elementi\n')

            if r['local_alphabet_count'] is not None:
                f.write(f'alfabeto locale: {r["local_alphabet_count"]} elementi\n')

            f.write(f'dizionario frammenti: {r["fragments_count"]} elementi\n')
            f.write(f'tempo: {r["elapsed_s"]}s\n')

            stdout = r['stdout']
            stderr = r['stderr']

            if stdout:
                f.write(f'stdout:\n{stdout}\n')

            if stderr:
                f.write(f'stderr:\n{stderr}\n')

            f.write('\n')

    print(f'\nLog completo scritto in: {LOG_PATH}')


if __name__ == '__main__':
    main()