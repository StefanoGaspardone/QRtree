import json
import subprocess
import os
import sys
import time
from datetime import datetime

import qrcode
from qrcode.util import QRData, MODE_8BIT_BYTE

SWEEP = [
    ('wifi.txt',          [['en']], [2]),
    ('switch.txt',        [['en']], [2]),
    ('defibrillator.txt', [['en']], [2]),
    ('mountain.txt',      [['en']], [2]),
    ('colonnina.txt',     [['en']], [2]),
]

MAX_DEPTH_AVAILABLE = 2 if sys.platform.startswith('linux') else 1

PYTHON = sys.executable

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TEST_DIR)
QRTREE_SCRIPT = os.path.join(PROJECT_ROOT, 'QRtree.py')
TEST_SUBDIR_NAME = os.path.basename(TEST_DIR)

LOG_PATH = os.path.join(TEST_DIR, 'log.txt')                  # tabella riassuntiva, una riga per run
DETAIL_PATH = os.path.join(TEST_DIR, 'log_details.txt')       # dettaglio di ogni run
RESULTS_PATH = os.path.join(TEST_DIR, 'results.jsonl')        # un risultato per riga, per riprendere un'esecuzione interrotta

ID_TO_LANGUAGE = {0: 'en', 1: 'it', 2: 'ch', 3: 'fr', 4: 'de'}

FINGERPRINT_BITS = 16

DICT_HEADER_CMD_BITS = 3

QRSCRIPT_HEADER_BITS = 1 + 4 + 1 + 4 + 4 + 1

QR_ERROR_CORRECTION = qrcode.constants.ERROR_CORRECT_H


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


def read_bits(bin_path):
    """Contenuto del .bin: una stringa di caratteri '0'/'1' (un carattere per bit)."""
    if not os.path.exists(bin_path):
        return None

    with open(bin_path, 'r') as f:
        return f.read().strip()


QR_BOX_SIZE = 10
QR_BORDER = 4


def png_qr_version(png_path):
    """Versione QR ricavata dalla larghezza in pixel del PNG generato (box_size 10, bordo 4): w = (17 + 4 * versione + 2 * bordo) * box_size.
    Serve a verificare che il calcolo fatto qui coincida con quello del QR code realmente generato."""
    if not os.path.exists(png_path):
        return None

    with open(png_path, 'rb') as f:
        header = f.read(24)

    if len(header) < 24 or header[:8] != b'\x89PNG\r\n\x1a\n':
        return None

    width = int.from_bytes(header[16:20], 'big')
    modules = width // QR_BOX_SIZE - 2 * QR_BORDER

    if width % QR_BOX_SIZE != 0 or (modules - 17) % 4 != 0:
        return None

    return (modules - 17) // 4


def qr_payload_bits(data_bits):
    """Bit effettivi scritti nel QR code: intestazione QRscript + dati + padding.
    Il padding (zeri seguiti da un 1) porta il totale a un multiplo di 8, come in encode()."""
    padding = (8 - (QRSCRIPT_HEADER_BITS + data_bits + 1)) % 8 + 1

    return QRSCRIPT_HEADER_BITS + data_bits + padding


def qr_version_for(payload_bytes):
    """Versione QR (con lo stesso livello di correzione di encode()) necessaria per payload_bytes byte,
    oppure None se supera la capacità massima. In modalità byte la versione dipende solo dalla lunghezza,
    quindi si usa un contenuto fittizio (non nullo: la libreria qrcode fallisce con dati di soli zeri)."""
    try:
        qr = qrcode.QRCode(version = None, error_correction = QR_ERROR_CORRECTION, box_size = 10, border = 4)
        qr.add_data(QRData(b'\xa5' * payload_bytes, MODE_8BIT_BYTE, False))
        qr.make(fit = True)

        return qr.version
    except Exception:
        return None


def extract_header_summary(bits):
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
    bits = read_bits(bin_path)
    bin_bits = len(bits) if bits is not None else None
    header_info = extract_header_summary(bits) or {}

    if bin_bits is not None:
        qr_bits = qr_payload_bits(bin_bits)
        qr_bytes = qr_bits // 8
        qr_version = qr_version_for(qr_bytes)
    else:
        qr_bits = None
        qr_bytes = None
        qr_version = None

    png_path = basename_abs + '.png'
    png_size = os.path.getsize(png_path) if os.path.exists(png_path) else None
    png_version = png_qr_version(png_path)

    cleanup_stale(basename_abs)

    return {
        'input_file': input_file,
        'languages': lang_arg,
        'max_depth': max_depth,
        'returncode': proc.returncode,
        'bin_bits': bin_bits,
        'qr_bits': qr_bits,
        'qr_bytes': qr_bytes,
        'qr_version': qr_version,
        'png_version': png_version,
        'version_ok': (png_version == qr_version) if png_version is not None and qr_version is not None else None,
        'png_size_bytes': png_size,
        'chain_order': header_info.get('chain_order'),
        'aux_count': header_info.get('aux_count'),
        'local_alphabet_count': header_info.get('local_alphabet_count'),
        'fragments_count': header_info.get('fragments_count'),
        'elapsed_s': round(elapsed, 2),
        'stdout': proc.stdout.strip(),
        'stderr': proc.stderr.strip(),
    }


ROW_FORMAT = '{:<25}{:<10}{:<10}{:<10}{:<12}{:<12}{:<10}{:<10}{:<12}{:<10}{:<20}{:<12}{:<12}'
HEADER_COLUMNS = ('File', 'Lingue', 'Max depth', 'Esito', 'Bin (bit)', 'QR (bit)', 'Versione', 'Verifica', 'Png (byte)', 'Tempo (s)', 'Ordine catena', 'Ausiliario', 'Frammenti')


def expand(sweep):
    runs = []

    for input_file, combos, depths in sweep:
        for languages in combos:
            for max_depth in depths:
                runs.append((input_file, languages, max_depth))

    return runs


def run_key(input_file, languages, max_depth):
    return f'{input_file}|{",".join(languages)}|{max_depth}'


def load_done():
    """Chiavi dei run già completati con successo, per non rifarli se l'esecuzione viene ripresa."""
    done = set()

    if os.path.exists(RESULTS_PATH):
        with open(RESULTS_PATH, 'r', encoding = 'utf-8') as f:
            for line in f:
                line = line.strip()

                if not line:
                    continue

                r = json.loads(line)

                if r.get('returncode') == 0:
                    done.add(run_key(r['input_file'], r['languages'].split(','), r['max_depth']))

    return done


def append_result(r, timestamp):
    """Scrive subito il risultato di un run, così un'interruzione non fa perdere quelli già completati."""
    with open(RESULTS_PATH, 'a', encoding = 'utf-8') as f:
        f.write(json.dumps(r) + '\n')

    new_table = not os.path.exists(LOG_PATH)

    with open(LOG_PATH, 'a', encoding = 'utf-8') as f:
        if new_table:
            f.write(f'QRtree encode sweep -- {timestamp}\n')
            f.write('=' * 70 + '\n\n')
            f.write(ROW_FORMAT.format(*HEADER_COLUMNS) + '\n')
            f.write('-' * 165 + '\n')

        esito = 'OK' if r['returncode'] == 0 else 'ERRORE'
        bin_str = str(r['bin_bits']) if r['bin_bits'] is not None else 'N/D'
        qr_str = str(r['qr_bits']) if r['qr_bits'] is not None else 'N/D'
        ver_str = str(r['qr_version']) if r['qr_version'] is not None else 'N/D'
        chk_str = {True: 'OK', False: 'DIVERSA', None: 'N/D'}[r['version_ok']]
        png_str = str(r['png_size_bytes']) if r['png_size_bytes'] is not None else 'N/D'
        chain_order = r['chain_order'] or 'N/D'
        aux_str = str(r['aux_count']) if r['aux_count'] is not None else '-'
        frag_str = str(r['fragments_count']) if r['fragments_count'] is not None else 'N/D'

        f.write(ROW_FORMAT.format(
            r['input_file'], r['languages'], r['max_depth'], esito, bin_str, qr_str, ver_str, chk_str, png_str, r['elapsed_s'], chain_order, aux_str, frag_str
        ) + '\n')

    with open(DETAIL_PATH, 'a', encoding = 'utf-8') as f:
        f.write(f'### {r["input_file"]} | lingue = {r["languages"]} | max_depth = {r["max_depth"]} ###\n')
        f.write(f'exit code: {r["returncode"]}\n')
        f.write(f'.bin: {r["bin_bits"]} bit\n')
        f.write(f'QR code: {r["qr_bits"]} bit (versione {r["qr_version"]}, versione del PNG {r["png_version"]}), con intestazione QRscript e padding\n')
        f.write(f'.png size: {r["png_size_bytes"]} byte\n')
        f.write(f'ordine catena: {r["chain_order"]}\n')

        if r['aux_count'] is not None:
            f.write(f'dizionario ausiliario: {r["aux_count"]} elementi\n')

        if r['local_alphabet_count'] is not None:
            f.write(f'alfabeto locale: {r["local_alphabet_count"]} elementi\n')

        f.write(f'dizionario frammenti: {r["fragments_count"]} elementi\n')
        f.write(f'tempo: {r["elapsed_s"]}s\n')

        if r['stdout']:
            f.write(f'stdout:\n{r["stdout"]}\n')

        if r['stderr']:
            f.write(f'stderr:\n{r["stderr"]}\n')

        f.write('\n')


def main():
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')

    done = load_done()

    runs = expand(SWEEP)
    print(f'Run in programma: {len(runs)} | già completati: {len(done)}')

    for input_file, languages, max_depth in runs:
        lang_label = '+'.join(languages)

        if max_depth > MAX_DEPTH_AVAILABLE:
            print(f'--- {input_file} | lingue = {lang_label} | max_depth = {max_depth} --- saltato (profondità non disponibile su questa piattaforma)')
            continue

        if run_key(input_file, languages, max_depth) in done:
            print(f'--- {input_file} | lingue = {lang_label} | max_depth = {max_depth} --- già fatto, saltato')
            continue

        print(f'--- {input_file} | lingue = {lang_label} | max_depth = {max_depth} ---')

        result = run_one(input_file, languages, max_depth)
        append_result(result, timestamp)

        returncode = result['returncode']
        status = 'OK' if returncode == 0 else f'ERRORE (exit {returncode})'
        bin_bits = result['bin_bits']
        qr_bits = result['qr_bits']
        qr_version = result['qr_version']
        png_size = result['png_size_bytes']
        check = {True: 'OK', False: 'DIVERSA (PNG v' + str(result['png_version']) + ')', None: 'N/D'}[result['version_ok']]
        bin_str = f'{bin_bits} bit' if bin_bits is not None else 'N/D'
        qr_str = f'{qr_bits} bit (v{qr_version})' if qr_bits is not None else 'N/D'
        png_str = f'{png_size} byte' if png_size is not None else 'N/D'

        print(f'  {status} | .bin = {bin_str} | QR = {qr_str} | verifica versione = {check} | .png = {png_str} | catena = {result["chain_order"]} | ausiliario = {result["aux_count"]} | frammenti = {result["fragments_count"]} | {result["elapsed_s"]}s')

    print(f'\nTabella: {LOG_PATH}\nDettaglio: {DETAIL_PATH}')


if __name__ == '__main__':
    main()