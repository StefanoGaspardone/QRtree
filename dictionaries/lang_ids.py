import hashlib

LANGUAGES = [
    {"code": "en", "url": None},
    {"code": "it", "url": "http://127.0.0.1:8899/it.bin"},
    {"code": "ch", "url": None},
    {"code": "fr", "url": None},
    {"code": "de", "url": None},
]

LANGUAGE_IDS = {lang["code"]: i for i, lang in enumerate(LANGUAGES)}
LANGUAGE_URLS = {lang["code"]: lang["url"] for lang in LANGUAGES}

FINGERPRINT_BITS = 16


def compute_fingerprint(lang_bits: str) -> int:
    digest = hashlib.sha256(lang_bits.encode('ascii')).digest()
    return int.from_bytes(digest[:2], 'big')