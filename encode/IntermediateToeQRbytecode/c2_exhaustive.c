#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
#else
    #include <libgen.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

/* ============================================================
 * Constants
 * ============================================================ */
static const uint8_t MAGIC[4] = { 'S', 'D', 'B', '1' };
#define VERSION 1

#define RAW 0
#define TOK 1

#define ENC_FIXED 0
#define ENC_HUFF_FREQ 1
#define ENC_HUFF_LEN 2
#define ENC_POSITIONAL 3

static int g_nthreads = 1;
static FILE *g_logfile = NULL;

static void log_line(const char *fmt, ...) {
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);

    vprintf(fmt, ap1);
    printf("\n");
    va_end(ap1);

    if(g_logfile) {
        vfprintf(g_logfile, fmt, ap2);
        fprintf(g_logfile, "\n");
        fflush(g_logfile);
    }

    va_end(ap2);
}

#define PROJECT_ROOT_LEVELS 4

static void get_executable_path(char *out, const size_t outsz) {
#ifdef _WIN32
    const DWORD n = GetModuleFileNameA(NULL, out, outsz);
    if(n == 0 || n == outsz) snprintf(out, outsz, ".");

    for(char *p = out; *p; p++) if(*p == '\\') *p = '/';
#else
    ssize_t n = readlink("/proc/self/exe", out, outsz - 1);

    if(n < 0) {
        if(!getcwd(out, outsz)) snprintf(out, outsz, ".");
    } else out[n] = '\0';
#endif
}

static void my_dirname(const char *path, char *out, const size_t outsz) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    while(len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) tmp[--len] = '\0';

    char *slash = strrchr(tmp, '/');
    char *bslash = strrchr(tmp, '\\');
    char *last = slash;

    if(bslash && (!last || bslash > last)) last = bslash;

    if(!last) {
        snprintf(out, outsz, ".");
    } else if(last == tmp) {
        snprintf(out, outsz, "%c", *last);
    } else {
        *last = '\0';
        snprintf(out, outsz, "%s", tmp);
    }
}

static void my_basename(const char *path, char *out, size_t outsz) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    while(len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) tmp[--len] = '\0';

    char *slash = strrchr(tmp, '/');
    char *bslash = strrchr(tmp, '\\');
    char *last = slash;

    if(bslash && (!last || bslash > last)) last = bslash;

    snprintf(out, outsz, "%s", last ? last + 1 : tmp);
}

static void get_project_root(char *out, const size_t outsz) {
    char cur[4096];
    get_executable_path(cur, sizeof(cur));

    for(int i = 0; i < PROJECT_ROOT_LEVELS; i++) {
        char next[4096];
        my_dirname(cur, next, sizeof(next));
        snprintf(cur, sizeof(cur), "%s", next);
    }

    snprintf(out, outsz, "%s", cur);
}

static void get_exe_stem(char *out, size_t outsz) {
    char exe_path[4096];
    get_executable_path(exe_path, sizeof(exe_path));

    char buf[1024];
    my_basename(exe_path, buf, sizeof(buf));

    char *dot = strrchr(buf, '.');
    if(dot) *dot = '\0';

    snprintf(out, outsz, "%s", buf);
}

static void path_stem(const char *path, char *out, const size_t outsz) {
    char tmp[4096];

    snprintf(tmp, sizeof(tmp), "%s", path);

    char *base = strrchr(tmp, '/');
    char *bslash = strrchr(tmp, '\\');

    if(bslash && (!base || bslash > base)) base = bslash;

    base = base ? base + 1 : tmp;
    char buf[1024];

    snprintf(buf, sizeof(buf), "%s", base);

    char *dot = strrchr(buf, '.');
    if(dot) *dot = '\0';

    snprintf(out, outsz, "%s", buf);
}

static void portable_mkdir_one(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0775);
#endif
}

static void mkdirs(const char *path) {
    char tmp[4096];

    snprintf(tmp, sizeof(tmp), "%s", path);
    const size_t len = strlen(tmp);

    if(len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for(char *p = tmp + 1; *p; p++) {
        if(*p == '/') {
            *p = '\0';
            portable_mkdir_one(tmp);
            *p = '/';
        }
    }

    portable_mkdir_one(tmp);
}

static void setup_logger(const char *input_filename) {
    char project_root[4096];
    get_project_root(project_root, sizeof(project_root));

    char logs_dir[4096];
    snprintf(logs_dir, sizeof(logs_dir), "%s/logs", project_root);
    mkdirs(logs_dir);

    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tmv);

    char script_name[256];
    get_exe_stem(script_name, sizeof(script_name));

    char base_input[256];
    path_stem(input_filename, base_input, sizeof(base_input));

    char log_filename[4096];
    snprintf(log_filename, sizeof(log_filename), "%s/%s_%s_%s.log", logs_dir, script_name, timestamp, base_input);

    g_logfile = fopen(log_filename, "w");
    if(!g_logfile) fprintf(stderr, "Warning: unable to open log file %s: %s\n", log_filename, strerror(errno));
}

/* ============================================================
 * Varint (LEB128 unsigned)
 * ============================================================ */
static size_t uvarint_encode(uint64_t x, uint8_t *out) {
    size_t n = 0;

    for(;;) {
        const uint8_t b = (uint8_t)(x & 0x7F);
        x >>= 7;
        out[n++] = x ? (uint8_t)(b | 0x80) : b;

        if(!x) break;
    }

    return n;
}

static uint64_t uvarint_decode(const uint8_t *data, size_t *pos) {
    uint64_t x = 0;
    int shift = 0;

    for(;;) {
        const uint8_t b = data[*pos];
        (*pos)++;
        x |= (uint64_t)(b & 0x7F) << shift;

        if(!(b & 0x80)) return x;

        shift += 7;
    }
}

static size_t varint_size(uint64_t x) {
    uint8_t tmp[16];
    return uvarint_encode(x, tmp);
}

static int needed_bits(int64_t n) {
    if(n <= 1) return 1;
    return (int)ceil(log2((double)n));
}

static int64_t exponential_ones_value(const int ones) {
    if(ones == 0) return 0;
    if(ones == 4) return (1LL << 4) - 1;
    return exponential_ones_value(ones / 2) + (1LL << (ones / 2)) - 1;
}

/* Mirrors compression.py's reference_encoding_size_bits: the QRscript
 * exponential-encoding cost of an unsigned int, used ONLY in scoring
 * (search decisions) so the C-built dictionary matches what the
 * actual QRtree bitstream will cost -- NOT used by the CLI's own
 * write_* functions, which keep using uvarint for the standalone
 * SDB1 file format. */
static int ref_enc_size_bits(int64_t value) {
    int length = 4;

    for(;;) {
        const int ones = (length == 4) ? 0 : length / 2;
        const int width = (length == 4) ? 4 : length / 2;
        const int64_t max_value = (1LL << width) - 1;
        const int64_t base = exponential_ones_value(ones);
        const int64_t cur_value = value - base;

        if(cur_value < max_value) return ones + width;

        length *= 2;
    }
}

/* ============================================================
 * BitWriter/Reader
 * ============================================================ */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    uint32_t acc;
    int nbits;
} BitWriter;

static void bw_init(BitWriter *bw) {
    bw->cap = 4096;
    bw->buf = (uint8_t *)malloc(bw->cap);
    bw->len = 0;
    bw->acc = 0;
    bw->nbits = 0;
}

static void bw_ensure(BitWriter *bw, const size_t extra) {
    if(bw->len + extra > bw->cap) {
        while(bw->len + extra > bw->cap) bw->cap *= 2;
        bw->buf = (uint8_t *)realloc(bw->buf, bw->cap);
    }
}

static void bw_push_byte(BitWriter *bw, uint8_t b) {
    bw_ensure(bw, 1);
    bw->buf[bw->len++] = b;
}

static void bw_write_bits(BitWriter *bw, const uint64_t value, const int n) {
    for(int i = n - 1; i >= 0; i--) {
        bw->acc = (bw->acc << 1) | (uint32_t)((value >> i) & 1ULL);
        bw->nbits += 1;

        if(bw->nbits == 8) {
            bw_push_byte(bw, (uint8_t)(bw->acc & 0xFF));
            bw->acc = 0;
            bw->nbits = 0;
        }
    }
}

static void bw_flush_to_byte(BitWriter *bw) {
    if(bw->nbits) {
        bw->acc <<= 8 - bw->nbits;
        bw_push_byte(bw, (uint8_t)(bw->acc & 0xFF));
        bw->acc = 0;
        bw->nbits = 0;
    }
}

static void bw_write_bytes_aligned(BitWriter *bw, const uint8_t *b, const size_t n) {
    bw_flush_to_byte(bw);
    bw_ensure(bw, n);
    memcpy(bw->buf + bw->len, b, n);
    bw->len += n;
}

static void bw_write_uvarint_aligned(BitWriter *bw, const uint64_t x) {
    uint8_t tmp[16];
    const size_t n = uvarint_encode(x, tmp);
    bw_write_bytes_aligned(bw, tmp, n);
}

static uint8_t *bw_getvalue(BitWriter *bw, size_t *outlen) {
    bw_flush_to_byte(bw);
    *outlen = bw->len;
    return bw->buf;
}

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t acc;
    int nbits;
} BitReader;

static void br_init(BitReader *br, const uint8_t *data, const size_t len, const size_t pos) {
    br->data = data;
    br->len = len;
    br->pos = pos;
    br->acc = 0;
    br->nbits = 0;
}

static uint64_t br_read_bits(BitReader *br, int n) {
    uint64_t v = 0;

    for(int i = 0; i < n; i++) {
        if(br->nbits == 0) {
            br->acc = br->data[br->pos];
            br->pos += 1;
            br->nbits = 8;
        }

        v = (v << 1) | (uint64_t)((br->acc >> (br->nbits - 1)) & 1);
        br->nbits -= 1;
    }

    return v;
}

static void br_align_to_byte(BitReader *br) {
    br->nbits = 0;
}

static const uint8_t *br_read_bytes_aligned(BitReader *br, const size_t n) {
    br_align_to_byte(br);
    const uint8_t *p = br->data + br->pos;
    br->pos += n;

    return p;
}

static uint64_t br_read_uvarint_aligned(BitReader *br) {
    return uvarint_decode(br->data, &br->pos);
}

/* ============================================================
 * Huffman
 * ============================================================ */
typedef struct {
    int64_t freq;
    int64_t tie;
    int *syms;
    int nsyms;
    int cap;
} HNode;

static HNode *hnode_new(const int64_t freq, const int64_t tie, const int sym) {
    HNode *n = malloc(sizeof(HNode));

    n->freq = freq;
    n->tie = tie;
    n->cap = 4;
    n->syms = (int *)malloc(sizeof(int) * n->cap);
    n->syms[0] = sym;
    n->nsyms = 1;

    return n;
}

static HNode *hnode_merge(const HNode *a, const HNode *b, const int64_t tie) {
    HNode *n = malloc(sizeof(HNode));

    n->freq = a->freq + b->freq;
    n->tie = tie;
    n->nsyms = a->nsyms + b->nsyms;
    n->cap = n->nsyms;
    n->syms = (int *)malloc(sizeof(int) * n->cap);

    memcpy(n->syms, a->syms, sizeof(int) * a->nsyms);
    memcpy(n->syms + a->nsyms, b->syms, sizeof(int) * b->nsyms);

    return n;
}

typedef struct {
    HNode **arr;
    int size;
    int cap;
} HHeap;

static void hheap_init(HHeap *h, const int cap) {
    h->cap = cap > 4 ? cap : 4;
    h->arr = (HNode **)malloc(sizeof(HNode *) * h->cap);
    h->size = 0;
}

static int hnode_less(const HNode *a, const HNode *b) {
    if (a->freq != b->freq) return a->freq < b->freq;
    return a->tie < b->tie;
}

static void hheap_push(HHeap *h, HNode *n) {
    if(h->size == h->cap) {
        h->cap *= 2;
        h->arr = (HNode **)realloc(h->arr, sizeof(HNode *) * h->cap);
    }

    int i = h->size++;
    h->arr[i] = n;

    while(i > 0) {
        const int p = (i - 1) / 2;

        if(hnode_less(h->arr[i], h->arr[p])) {
            HNode *tmp = h->arr[i];
            h->arr[i] = h->arr[p];
            h->arr[p] = tmp;

            i = p;
        } else break;
    }
}

static HNode *hheap_pop(HHeap *h) {
    HNode *top = h->arr[0];

    h->size--;
    h->arr[0] = h->arr[h->size];

    int i = 0;
    for(;;) {
        const int l = 2 * i + 1, r = 2 * i + 2;
        int smallest = i;

        if(l < h->size && hnode_less(h->arr[l], h->arr[smallest])) smallest = l;
        if(r < h->size && hnode_less(h->arr[r], h->arr[smallest])) smallest = r;

        if(smallest == i) break;

        HNode *tmp = h->arr[i];
        h->arr[i] = h->arr[smallest];
        h->arr[smallest] = tmp;

        i = smallest;
    }

    return top;
}

static int *huffman_lengths(const int64_t *freq, const int count) {
    if(count == 0) return NULL;

    int *lengths = calloc(count, sizeof(int));

    if(count == 1) {
        lengths[0] = 1;
        return lengths;
    }

    HHeap heap;
    hheap_init(&heap, count);

    for(int s = 0; s < count; s++) hheap_push(&heap, hnode_new(freq[s], s, s));

    int64_t tie = count;
    while(heap.size > 1) {
        HNode *n1 = hheap_pop(&heap);
        HNode *n2 = hheap_pop(&heap);

        for(int i = 0; i < n1->nsyms; i++) lengths[n1->syms[i]] += 1;
        for(int i = 0; i < n2->nsyms; i++) lengths[n2->syms[i]] += 1;

        HNode *merged = hnode_merge(n1, n2, tie++);
        hheap_push(&heap, merged);

        free(n1->syms); free(n1);
        free(n2->syms); free(n2);
    }

    HNode *last = hheap_pop(&heap);

    free(last->syms);
    free(last);
    free(heap.arr);

    return lengths;
}

typedef struct { uint32_t code; int length; } HCode;
typedef struct { int sym; int length; } SortItem;

static int sortitem_cmp(const void *a, const void *b) {
    const SortItem *x = a, *y = b;

    if(x->length != y->length) return x->length - y->length;
    return x->sym - y->sym;
}

static HCode *canonical_codes(const int *lengths, const int count) {
    HCode *codes = calloc(count, sizeof(HCode));
    SortItem *items = malloc(sizeof(SortItem) * count);

    for(int i = 0; i < count; i++) {
        items[i].sym = i;
        items[i].length = lengths[i];
    }
    qsort(items, count, sizeof(SortItem), sortitem_cmp);

    uint32_t code = 0;
    int prev = 0;
    for(int i = 0; i < count; i++) {
        const int s = items[i].sym, L = items[i].length;

        if(L > prev) code <<= L - prev;

        codes[s].code = code;
        codes[s].length = L;
        code += 1;

        prev = L;
    }

    free(items);
    return codes;
}

typedef struct {
    uint64_t key;
    int sym;
    int used;
} LookupEntry;

typedef struct {
    LookupEntry *entries;
    size_t cap;
    int max_len;
} HLookup;

static uint64_t lookup_hash(uint64_t key, const size_t cap) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;

    return key % cap;
}

static void hlookup_insert(const HLookup *lk, const uint64_t key, const int sym) {
    size_t idx = lookup_hash(key, lk->cap);

    while(lk->entries[idx].used) idx = (idx + 1) % lk->cap;

    lk->entries[idx].key = key;
    lk->entries[idx].sym = sym;
    lk->entries[idx].used = 1;
}

static int hlookup_find(const HLookup *lk, const uint64_t key, int *found) {
    size_t idx = lookup_hash(key, lk->cap);
    const size_t start = idx;

    while(lk->entries[idx].used) {
        if(lk->entries[idx].key == key) {
            *found = 1;
            return lk->entries[idx].sym;
        }

        idx = (idx + 1) % lk->cap;
        if(idx == start) break;
    }

    *found = 0;
    return -1;
}

static HLookup canonical_lookup(const int *lengths, const int count) {
    HLookup lk;

    lk.cap = (size_t)(count * 2 + 8);
    lk.entries = (LookupEntry *)calloc(lk.cap, sizeof(LookupEntry));
    lk.max_len = 0;

    SortItem *items = malloc(sizeof(SortItem) * count);
    for (int i = 0; i < count; i++) {
        items[i].sym = i;
        items[i].length = lengths[i];
    }
    qsort(items, count, sizeof(SortItem), sortitem_cmp);

    uint32_t code = 0;
    int prev = 0;
    for(int i = 0; i < count; i++) {
        const int s = items[i].sym, L = items[i].length;

        if(L > prev) code <<= L - prev;

        const uint64_t key = ((uint64_t)code << 6) | (uint64_t)L;
        hlookup_insert(&lk, key, s);
        code += 1;
        prev = L;

        if(L > lk.max_len) lk.max_len = L;
    }

    free(items);
    return lk;
}

static void hlookup_free(HLookup *lk) {
    free(lk->entries);
    lk->entries = NULL;
}

static uint8_t *normalize_freqs(const int64_t *freq, const int n) {
    uint8_t *out = malloc(n > 0 ? n : 1);
    int64_t max_f = 1;

    if(n > 0) {
        max_f = freq[0];
        for(int i = 1; i < n; i++) if (freq[i] > max_f) max_f = freq[i];

        if(max_f <= 0) max_f = 1;
    }

    for(int i = 0; i < n; i++) {
        const double v = (double)freq[i] * 255.0 / (double)max_f, fl = floor(v), diff = v - fl;
        long r;

        if(diff < 0.5) r = (long)fl;
        else if(diff > 0.5) r = (long)fl + 1;
        else {
           const long lo = (long)fl;
            r = lo % 2 == 0 ? lo : lo + 1;
        }

        if(r < 1) r = 1;
        if(r > 255) r = 255;

        out[i] = (uint8_t)r;
    }
    return out;
}

/* ============================================================
 * Elias gamma
 * ============================================================ */
static int elias_bitlen(const uint64_t n_plus1_bits) {
    int k = 0;
    uint64_t v = n_plus1_bits;

    while(v) {
        k++;
        v >>= 1;
    }

    return k;
}

static int elias_length(const int64_t i) {
    const uint64_t n = (uint64_t)i + 1;
    const int bl = elias_bitlen(n);
    const int k = bl - 1;

    return 2 * k + 1;
}

static void elias_write(BitWriter *bw, const int64_t i) {
    const uint64_t n = (uint64_t)i + 1;
    const int bl = elias_bitlen(n);
    const int k = bl - 1;

    bw_write_bits(bw, n, 2 * k + 1);
}

static int64_t elias_read(BitReader *br) {
    int k = 0;

    while(br_read_bits(br, 1) == 0) k++;

    if(k == 0) return 0;

    const uint64_t v = (1ULL << k) | br_read_bits(br, k);
    return (int64_t)v - 1;
}

/* ============================================================
 * CODECS
 * ============================================================ */
typedef struct {
    int is_huffman;
    int bits;
    HLookup lookup;
    int max_len;
    int has_lookup;
} Decoder;

static void decoder_free(Decoder *d) {
    if(d->has_lookup) hlookup_free(&d->lookup);
}

static int *uniform_lengths(const int count) {
    if(count == 0) return NULL;

    const int bits = needed_bits(count);
    int *out = malloc(sizeof(int) * count);

    for(int i = 0; i < count; i++) out[i] = bits;

    return out;
}

static int *huffman_freq_lengths(const int64_t *freq_raw, const int count) {
    if(count == 0) return NULL;

    int64_t *full = malloc(sizeof(int64_t) * count);
    for(int i = 0; i < count; i++) full[i] = freq_raw[i] != 0 ? freq_raw[i] : 1;

    uint8_t *norm = normalize_freqs(full, count);
    int64_t *norm64 = malloc(sizeof(int64_t) * count);

    for(int i = 0; i < count; i++) norm64[i] = norm[i];

    int *lengths = huffman_lengths(norm64, count);

    free(full); free(norm); free(norm64);
    return lengths;
}

static int *huffman_len_lengths(const int64_t *freq_raw, const int count) {
    if(count == 0) return NULL;

    int64_t *full = malloc(sizeof(int64_t) * count);
    for(int i = 0; i < count; i++) full[i] = freq_raw[i] != 0 ? freq_raw[i] : 1;

    int *lengths = huffman_lengths(full, count);
    free(full);

    int max_len_found = 0;
    for(int i = 0; i < count; i++) if(lengths[i] > max_len_found) max_len_found = lengths[i];

    if(max_len_found <= 15) return lengths;

    for(int i = 0; i < count; i++) if(lengths[i] > 15) lengths[i] = 15;

    const int64_t target = 1LL << 15;
    for(;;) {
        int64_t kraft_sum = 0;
        for(int i = 0; i < count; i++) kraft_sum += (1LL << (15 - lengths[i]));

        if(kraft_sum <= target) break;

        int best_sym = -1;
        for(int i = 0; i < count; i++) {
            if(lengths[i] >= 15) continue;
            
            if(best_sym < 0 || lengths[i] > lengths[best_sym] ||
               (lengths[i] == lengths[best_sym] && i > best_sym)) {
                best_sym = i;
            }
        }

        lengths[best_sym] += 1;
    }

    return lengths;
}

static int *codec_char_lengths(const int encoding, const int A, const int64_t *char_freq_raw) {
    switch(encoding) {
        case ENC_FIXED: return uniform_lengths(A);
        case ENC_POSITIONAL: {
            if(A == 0) return NULL;

            int *out = malloc(sizeof(int) * A);
            for(int i = 0; i < A; i++) out[i] = elias_length(i);

            return out;
        }
        case ENC_HUFF_FREQ: return huffman_freq_lengths(char_freq_raw, A);
        case ENC_HUFF_LEN: return huffman_len_lengths(char_freq_raw, A);
    }

    return NULL;
}

static int *codec_token_lengths(const int encoding, const int64_t *tok_freq_raw, const int D) {
    switch(encoding) {
        case ENC_FIXED: return uniform_lengths(D);
        case ENC_POSITIONAL: {
            if (D == 0) return NULL;

            typedef struct { int idx; int64_t f; } RF;

            RF *rf = malloc(sizeof(RF) * D);
            for(int i = 0; i < D; i++) {
                rf[i].idx = i;
                rf[i].f = tok_freq_raw[i];
            }

            for(int i = 1; i < D; i++) {
                const RF key = rf[i];
                int j = i - 1;

                while(j >= 0 && rf[j].f < key.f) {
                    rf[j+1] = rf[j];
                    j--;
                }

                rf[j+1] = key;
            }

            int *out = malloc(sizeof(int) * D);
            for(int rank = 0; rank < D; rank++) out[rf[rank].idx] = elias_length(rank);

            free(rf);
            return out;
        }
        case ENC_HUFF_FREQ: return huffman_freq_lengths(tok_freq_raw, D);
        case ENC_HUFF_LEN: return huffman_len_lengths(tok_freq_raw, D);
    }
    return NULL;
}

static void codec_write_symbol(const int encoding, BitWriter *bw, const int sym_id, const HCode *codes, const int fixed_bits) {
    switch(encoding) {
        case ENC_FIXED:
            bw_write_bits(bw, (uint64_t)sym_id, fixed_bits);
            break;
        case ENC_POSITIONAL:
            elias_write(bw, sym_id);
            break;
        case ENC_HUFF_FREQ:
        case ENC_HUFF_LEN:
            bw_write_bits(bw, codes[sym_id].code, codes[sym_id].length);
            break;
    }
}

static int codec_read_symbol(const int encoding, BitReader *br, const Decoder *dec) {
    switch(encoding) {
        case ENC_FIXED:
            return (int)br_read_bits(br, dec->bits);
        case ENC_POSITIONAL:
            return (int)elias_read(br);
        case ENC_HUFF_FREQ:
        case ENC_HUFF_LEN: {
            uint64_t cur = 0;

            for (int L = 1; L <= dec->max_len; L++) {
                cur = (cur << 1) | br_read_bits(br, 1);
                const uint64_t key = (cur << 6) | (uint64_t)L;
                int found = 0;
                const int sym = hlookup_find(&dec->lookup, key, &found);

                if(found) return sym;
            }

            fprintf(stderr, "Error: invalid Huffman code\n");
            exit(1);
        }
    }
    return -1;
}

static HCode *codec_encode_codes_from_lengths(const int encoding, const int *lengths, const int count) {
    switch(encoding) {
        case ENC_FIXED:
        case ENC_POSITIONAL:
            return NULL;
        case ENC_HUFF_FREQ:
        case ENC_HUFF_LEN:
            if(count == 0) return NULL;
            return canonical_codes(lengths, count);
    }

    return NULL;
}

static Decoder codec_decoder_from_lengths(const int encoding, const int *lengths, const int count) {
    Decoder d = {0};

    switch(encoding) {
        case ENC_FIXED:
            d.bits = needed_bits(count);
            break;
        case ENC_POSITIONAL:
            break;
        case ENC_HUFF_FREQ:
        case ENC_HUFF_LEN:
            if(count > 0) {
                d.lookup = canonical_lookup(lengths, count);
                d.max_len = d.lookup.max_len;
                d.has_lookup = 1;
            }

            break;
    }

    return d;
}

static void codec_write_overhead(const int encoding, BitWriter *bw, const int64_t *freq_raw, const int count) {
    switch(encoding) {
        case ENC_FIXED:
        case ENC_POSITIONAL:
            break;
        case ENC_HUFF_FREQ: {
            int64_t *full = malloc(sizeof(int64_t) * (count > 0 ? count : 1));
            for(int i = 0; i < count; i++) full[i] = freq_raw[i] != 0 ? freq_raw[i] : 1;

            uint8_t *norm = normalize_freqs(full, count);
            bw_write_bytes_aligned(bw, norm, count);

            free(full); free(norm);
            break;
        }
        case ENC_HUFF_LEN: {
            bw_flush_to_byte(bw);
            int *lens = count > 0 ? huffman_len_lengths(freq_raw, count) : NULL;

            for(int i = 0; i < count; i++) {
                const int L = lens ? lens[i] : 1;
                bw_write_bits(bw, (uint64_t)L, 4);
            }
            bw_flush_to_byte(bw);

            free(lens);
            break;
        }
    }
}

static int *codec_read_overhead(const int encoding, BitReader *br, const int count) {
    switch(encoding) {
        case ENC_FIXED:
        case ENC_POSITIONAL:
            return NULL;
        case ENC_HUFF_FREQ: {
            const uint8_t *freq_bytes = br_read_bytes_aligned(br, count);
            int64_t *norm = malloc(sizeof(int64_t) * (count > 0 ? count : 1));

            for(int i = 0; i < count; i++) norm[i] = freq_bytes[i];
            int *lengths = huffman_lengths(norm, count);

            free(norm);
            return lengths;
        }
        case ENC_HUFF_LEN: {
            br_align_to_byte(br);
            int *lens = malloc(sizeof(int) * (count > 0 ? count : 1));

            for(int i = 0; i < count; i++) lens[i] = (int)br_read_bits(br, 4);
            br_align_to_byte(br);

            return lens;
        }
    }
    return NULL;
}

static int codec_overhead_bits(const int encoding, const int count) {
    switch(encoding) {
        case ENC_FIXED:
        case ENC_POSITIONAL:
            return 0;
        case ENC_HUFF_FREQ:
            return count * 8;
        case ENC_HUFF_LEN:
            return count * 4;
    }

    return 0;
}

static void compute_char_bit_lengths(const uint8_t *alphabet, const int A, const int *char_lengths_by_id, int *byte_len_out /* [256] */) {
    for(int i = 0; i < A; i++) byte_len_out[alphabet[i]] = char_lengths_by_id[i];
}

/* ============================================================
 * I/O strings
 * ============================================================ */
typedef struct { uint8_t *data; size_t len; size_t cap; } ByteBuf;

static void bytebuf_init(ByteBuf *b, const size_t cap) {
    b->cap = cap > 0 ? cap : 64;
    b->data = (uint8_t *)malloc(b->cap);
    b->len = 0;
}

static void bytebuf_push(ByteBuf *b, const uint8_t c) {
    if(b->len == b->cap) {
        b->cap *= 2;
        b->data = (uint8_t *)realloc(b->data, b->cap);
    }

    b->data[b->len++] = c;
}

static void utf8_encode_cp(const uint32_t cp, ByteBuf *out) {
    if(cp <= 0x7F) {
        bytebuf_push(out, (uint8_t)cp);
    } else if(cp <= 0x7FF) {
        bytebuf_push(out, (uint8_t)(0xC0 | (cp >> 6)));
        bytebuf_push(out, (uint8_t)(0x80 | (cp & 0x3F)));
    } else if(cp <= 0xFFFF) {
        bytebuf_push(out, (uint8_t)(0xE0 | (cp >> 12)));
        bytebuf_push(out, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        bytebuf_push(out, (uint8_t)(0x80 | (cp & 0x3F)));
    } else {
        bytebuf_push(out, (uint8_t)(0xF0 | (cp >> 18)));
        bytebuf_push(out, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
        bytebuf_push(out, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        bytebuf_push(out, (uint8_t)(0x80 | (cp & 0x3F)));
    }
}

static int hexval(const char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;

    return -1;
}

static uint8_t *try_decode_python_string_literal(const char *line, const size_t linelen, size_t *outlen) {
    if(linelen < 2) return NULL;

    const char q = line[0];

    if(q != '\'' && q != '"') return NULL;
    if(line[linelen - 1] != q) return NULL;

    ByteBuf out;
    bytebuf_init(&out, linelen * 4 + 4);

    size_t i = 1;
    const size_t end = linelen - 1;

    while(i < end) {
        const char c = line[i];

        if(c == q) {
            free(out.data);
            return NULL;
        }

        if(c == '\\') {
            if(i + 1 >= end) {
                free(out.data);
                return NULL;
            }

            const char e = line[i + 1];
            switch(e) {
                case '\\': bytebuf_push(&out, '\\'); i += 2; break;
                case '\'': bytebuf_push(&out, '\''); i += 2; break;
                case '"':  bytebuf_push(&out, '"');  i += 2; break;
                case 'n':  bytebuf_push(&out, '\n'); i += 2; break;
                case 't':  bytebuf_push(&out, '\t'); i += 2; break;
                case 'r':  bytebuf_push(&out, '\r'); i += 2; break;
                case 'b':  bytebuf_push(&out, '\b'); i += 2; break;
                case 'f':  bytebuf_push(&out, '\f'); i += 2; break;
                case 'v':  bytebuf_push(&out, '\v'); i += 2; break;
                case 'a':  bytebuf_push(&out, '\a'); i += 2; break;
                case '0':  bytebuf_push(&out, '\0'); i += 2; break;
                case '\n': i += 2; break;
                case 'x': {
                    if(i + 3 >= end + 1 && i + 4 > end) {
                        free(out.data);
                        return NULL;
                    }

                    if(i + 4 > end + 1) {
                        free(out.data);
                        return NULL;
                    }

                    const int h1 = hexval(line[i + 2]), h2 = hexval(line[i + 3]);
                    if(h1 < 0 || h2 < 0) {
                        free(out.data);
                        return NULL;
                    }

                    utf8_encode_cp((uint32_t)(h1 * 16 + h2), &out);
                    i += 4;

                    break;
                }
                case 'u': {
                    if(i + 6 > end + 1) { free(out.data); return NULL; }

                    int h[4];
                    for(int k = 0; k < 4; k++) {
                        h[k] = hexval(line[i + 2 + k]);
                        if(h[k] < 0) {
                            free(out.data);
                            return NULL;
                        }
                    }

                    const uint32_t cp = (h[0] << 12) | (h[1] << 8) | (h[2] << 4) | h[3];
                    utf8_encode_cp(cp, &out);
                    i += 6;

                    break;
                }
                case 'U': {
                    if(i + 10 > end + 1) {
                        free(out.data);
                        return NULL;
                    }

                    uint32_t cp = 0;
                    for(int k = 0; k < 8; k++) {
                        const int hv = hexval(line[i + 2 + k]);
                        if(hv < 0) {
                            free(out.data);
                            return NULL;
                        }

                        cp = (cp << 4) | (uint32_t)hv;
                    }

                    utf8_encode_cp(cp, &out);
                    i += 10;

                    break;
                }
                default:
                    if(e >= '0' && e <= '7') {
                        int val = 0, k = 0;
                        size_t j = i + 1;

                        while(k < 3 && j < end && line[j] >= '0' && line[j] <= '7') {
                            val = val * 8 + (line[j] - '0');
                            j++; k++;
                        }

                        bytebuf_push(&out, (uint8_t)(val & 0xFF));
                        i = j;
                    } else {
                        bytebuf_push(&out, '\\');
                        bytebuf_push(&out, (uint8_t)e);

                        i += 2;
                    }

                    break;
            }
        } else {
            bytebuf_push(&out, (uint8_t)c);
            i += 1;
        }
    }

    *outlen = out.len;
    return out.data;
}

typedef struct { uint8_t *data; size_t len; } StrItem;

static long my_getline(char **lineptr, size_t *n, FILE *stream) {
    if(*lineptr == NULL || *n == 0) {
        *n = 256;
        *lineptr = (char *)malloc(*n);
    }

    size_t len = 0;
    int c;

    while((c = fgetc(stream)) != EOF) {
        if(len + 1 >= *n) {
            *n *= 2;
            *lineptr = (char *)realloc(*lineptr, *n);
        }

        (*lineptr)[len++] = (char)c;

        if(c == '\n') break;
    }

    if(len == 0 && c == EOF) return -1;

    (*lineptr)[len] = '\0';
    return len;
}

static StrItem *read_strings_text(const char *inputs_dir, const char *path, int *out_count) {
    char full_path[4096];
    snprintf(full_path, sizeof(full_path), "%s/%s", inputs_dir, path);

    FILE *f = fopen(full_path, "r");
    if(!f) {
        fprintf(stderr, "Error: unable to open %s: %s\n", full_path, strerror(errno));
        exit(1);
    }

    StrItem *items = NULL;
    int cap = 0, n = 0;

    char *line = NULL;
    size_t linecap = 0;
    long linelen;

    while((linelen = my_getline(&line, &linecap, f)) != -1) {
        while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        line[linelen] = '\0';

        if(linelen == 0) continue;

        if(n == cap) {
            cap = cap ? cap * 2 : 64;
            items = (StrItem *)realloc(items, sizeof(StrItem) * cap);
        }

        size_t declen = 0;
        uint8_t *decoded = try_decode_python_string_literal(line, (size_t)linelen, &declen);
        if(decoded) {
            items[n].data = decoded;
            items[n].len = declen;
        } else {
            items[n].data = (uint8_t *)malloc((size_t)linelen);
            memcpy(items[n].data, line, (size_t)linelen);
            items[n].len = (size_t)linelen;
        }

        n++;
    }

    free(line);
    fclose(f);

    *out_count = n;
    return items;
}

/* ============================================================
 * Alphabet
 * ============================================================ */
typedef struct {
    uint8_t alphabet[256];
    int A;
    int byte_to_id[256];
    int char_bits;
    int64_t char_freq_by_byte[256];
} Alphabet;

static void build_alphabet(const StrItem *strs, const int n, const int sort_by_freq, Alphabet *out) {
    int64_t freq[256] = {0};
    int present[256] = {0};

    for(int s = 0; s < n; s++) {
        for(size_t k = 0; k < strs[s].len; k++) {
            const uint8_t bv = strs[s].data[k];
            freq[bv]++;
            present[bv] = 1;
        }
    }

    int idxs[256]; int A = 0;
    for(int b = 0; b < 256; b++) if (present[b]) idxs[A++] = b;

    if(sort_by_freq) {
        int first_seen_order[256]; int m = 0;
        int seen[256] = {0};

        for(int s = 0; s < n; s++) {
            for(size_t k = 0; k < strs[s].len; k++) {
                const uint8_t bv = strs[s].data[k];
                if(!seen[bv]) {
                    seen[bv] = 1;
                    first_seen_order[m++] = bv;
                }
            }
        }

        for(int i = 1; i < m; i++) {
            const int key = first_seen_order[i];
            const int64_t kf = freq[key];
            int j = i - 1;

            while(j >= 0 && freq[first_seen_order[j]] < kf) {
                first_seen_order[j+1] = first_seen_order[j];
                j--;
            }

            first_seen_order[j+1] = key;
        }

        for(int i = 0; i < m; i++) idxs[i] = first_seen_order[i];
        A = m;
    }

    out->A = A;
    for(int i = 0; i < 256; i++) out->byte_to_id[i] = -1;

    for(int i = 0; i < A; i++) {
        out->alphabet[i] = (uint8_t)idxs[i];
        out->byte_to_id[idxs[i]] = i;
    }

    out->char_bits = needed_bits(A);
    memcpy(out->char_freq_by_byte, freq, sizeof(freq));
}

static void alphabet_char_freq_by_id(const Alphabet *alph, int64_t *out /* [A] */) {
    for(int i = 0; i < alph->A; i++) out[i] = alph->char_freq_by_byte[alph->alphabet[i]];
}

/* ============================================================
 * Dictionary
 * ============================================================ */
typedef struct { uint8_t type; int32_t val; } Sym;

typedef struct { Sym *items; int len; int cap; } Seq;

static void seq_init(Seq *s, const int cap) {
    s->cap = cap > 0 ? cap : 4;
    s->items = (Sym *)malloc(sizeof(Sym) * s->cap);
    s->len = 0;
}

static void seq_push(Seq *s, const uint8_t type, const int32_t val) {
    if(s->len == s->cap) {
        s->cap *= 2;
        s->items = (Sym *)realloc(s->items, sizeof(Sym) * s->cap);
    }

    s->items[s->len].type = type;
    s->items[s->len].val = val;
    s->len++;
}

static void seq_free(Seq *s) {
    free(s->items);

    s->items = NULL;
    s->len = 0;
    s->cap = 0;
}

static Seq seq_clone(const Seq *src) {
    Seq d;

    d.cap = src->len > 0 ? src->len : 1;
    d.items = (Sym *)malloc(sizeof(Sym) * d.cap);

    memcpy(d.items, src->items, sizeof(Sym) * src->len);

    d.len = src->len;
    return d;
}

typedef struct { Seq *seqs; int n; } SeqList;

static SeqList seqlist_clone(const SeqList *src) {
    SeqList d; d.n = src->n;
    d.seqs = (Seq *)malloc(sizeof(Seq) * d.n);

    for(int i = 0; i < d.n; i++) d.seqs[i] = seq_clone(&src->seqs[i]);

    return d;
}

static void seqlist_free(SeqList *sl) {
    for(int i = 0; i < sl->n; i++) seq_free(&sl->seqs[i]);

    free(sl->seqs);
    sl->seqs = NULL; sl->n = 0;
}

static SeqList initial_sequences(const StrItem *strs, const int n) {
    SeqList sl; sl.n = n;
    sl.seqs = (Seq *)malloc(sizeof(Seq) * n);

    for(int i = 0; i < n; i++) {
        seq_init(&sl.seqs[i], (int)strs[i].len);
        for(size_t k = 0; k < strs[i].len; k++) seq_push(&sl.seqs[i], RAW, strs[i].data[k]);
    }

    return sl;
}

typedef struct { uint8_t *data; int len; } DictEntry;

typedef struct { DictEntry *entries; int n; int cap; } Dictionary;

static void dict_init(Dictionary *d, const int cap) {
    d->cap = cap > 0 ? cap : 4;
    d->entries = (DictEntry *)malloc(sizeof(DictEntry) * d->cap);
    d->n = 0;
}

static void dict_push(Dictionary *d, const uint8_t *bytes, const int len) {
    if(d->n == d->cap) {
        d->cap *= 2;
        d->entries = (DictEntry *)realloc(d->entries, sizeof(DictEntry) * d->cap);
    }

    d->entries[d->n].data = (uint8_t *)malloc(len > 0 ? len : 1);

    memcpy(d->entries[d->n].data, bytes, len);

    d->entries[d->n].len = len;
    d->n++;
}

static Dictionary dict_clone(const Dictionary *src) {
    Dictionary d; dict_init(&d, src->n > 0 ? src->n : 4);

    for(int i = 0; i < src->n; i++) dict_push(&d, src->entries[i].data, src->entries[i].len);

    return d;
}

static void dict_free(Dictionary *d) {
    for(int i = 0; i < d->n; i++) free(d->entries[i].data);

    free(d->entries);
    d->entries = NULL; d->n = 0; d->cap = 0;
}

typedef struct {
    uint8_t *key;
    int keylen;
    int64_t count;
} CandEntry;

typedef struct {
    CandEntry *entries;
    int n;
    int cap;
    int *table;
    size_t tcap;
} CandMap;

static uint64_t fnv1a(const uint8_t *data, const int len) {
    uint64_t h = 1469598103934665603ULL;

    for(int i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }

    return h;
}

static void candmap_init(CandMap *m, const int cap) {
    m->cap = cap > 0 ? cap : 64;
    m->entries = (CandEntry *)malloc(sizeof(CandEntry) * m->cap);
    m->n = 0;
    m->tcap = (size_t)(m->cap * 2);
    m->table = (int *)calloc(m->tcap, sizeof(int));
}

static void candmap_free(CandMap *m) {
    for(int i = 0; i < m->n; i++) free(m->entries[i].key);

    free(m->entries);
    free(m->table);

    m->entries = NULL;
    m->table = NULL;
    m->n = 0;
    m->cap = 0;
    m->tcap = 0;
}

static void candmap_rehash(CandMap *m, const size_t newtcap) {
    int *newtable = calloc(newtcap, sizeof(int));

    for(int i = 0; i < m->n; i++) {
        const uint64_t h = fnv1a(m->entries[i].key, m->entries[i].keylen);
        size_t idx = h % newtcap;

        while(newtable[idx]) idx = (idx + 1) % newtcap;

        newtable[idx] = i + 1;
    }

    free(m->table);

    m->table = newtable;
    m->tcap = newtcap;
}

static void candmap_grow_entries(CandMap *m) {
    m->cap *= 2;
    m->entries = (CandEntry *)realloc(m->entries, sizeof(CandEntry) * m->cap);
}

static void candmap_incr(CandMap *m, const uint8_t *key, const int keylen, const int64_t incr) {
    if(m->n * 2 >= (int)m->tcap) candmap_rehash(m, m->tcap * 2);

    const uint64_t h = fnv1a(key, keylen);
    size_t idx = h % m->tcap;

    for(;;) {
        const int slot = m->table[idx];

        if(slot == 0) break;

        CandEntry *e = &m->entries[slot - 1];
        if(e->keylen == keylen && memcmp(e->key, key, keylen) == 0) {
            e->count += incr;
            return;
        }

        idx = (idx + 1) % m->tcap;
    }

    if(m->n == m->cap) candmap_grow_entries(m);

    const int newidx = m->n;

    m->entries[newidx].key = (uint8_t *)malloc(keylen > 0 ? keylen : 1);

    memcpy(m->entries[newidx].key, key, keylen);

    m->entries[newidx].keylen = keylen;
    m->entries[newidx].count = incr;
    m->n++;
    m->table[idx] = newidx + 1;
}

static void candmap_merge_from(CandMap *dst, const CandMap *src) {
    for(int i = 0; i < src->n; i++) candmap_incr(dst, src->entries[i].key, src->entries[i].keylen, src->entries[i].count);
}

static int seq_is_raw_run_and_get(const Seq *seq, const int i, uint8_t *out_byte) {
    if(i < 0 || i >= seq->len) return 0;
    if(seq->items[i].type != RAW) return 0;

    *out_byte = (uint8_t)seq->items[i].val;

    return 1;
}

static void find_candidates_range(const SeqList *sl, const int seq_from, const int seq_to, const int min_len, const int max_len, CandMap *m) {
    uint8_t *acc = malloc((size_t)(max_len > 0 ? max_len : 1));

    for(int si = seq_from; si < seq_to; si++) {
        const Seq *seq = &sl->seqs[si];
        const int n = seq->len;

        for(int i = 0; i < n; i++) {
            if(seq->items[i].type != RAW) continue;

            int acclen = 0;
            const int jmax = n < i + max_len ? n : i + max_len;
            for(int j = i; j < jmax; j++) {
                if(seq->items[j].type != RAW) break;

                acc[acclen] = (uint8_t)seq->items[j].val;
                acclen++;

                if(acclen >= min_len) candmap_incr(m, acc, acclen, 1);
            }
        }
    }

    free(acc);
}

typedef struct {
    const SeqList *sl;
    int seq_from, seq_to;
    int min_len, max_len;
    CandMap local;
} FindCandArg;

static void *find_candidates_thread(void *arg) {
    FindCandArg *a = arg;

    candmap_init(&a->local, 256);
    find_candidates_range(a->sl, a->seq_from, a->seq_to, a->min_len, a->max_len, &a->local);

    return NULL;
}

static CandMap find_candidates(const SeqList *sl, const int min_len, const int max_len) {
    CandMap full;

    int nthreads = g_nthreads;
    if(nthreads > sl->n) nthreads = sl->n > 0 ? sl->n : 1;
    if(nthreads < 1) nthreads = 1;

    if(nthreads <= 1 || sl->n < 32) {
        candmap_init(&full, 256);
        find_candidates_range(sl, 0, sl->n, min_len, max_len, &full);
    } else {
        pthread_t threads[nthreads];
        FindCandArg args[nthreads];

        const int base = sl->n / nthreads, rem = sl->n % nthreads;
        int start = 0;

        for(int t = 0; t < nthreads; t++) {
            const int cnt = base + (t < rem ? 1 : 0);

            args[t].sl = sl;
            args[t].seq_from = start;
            args[t].seq_to = start + cnt;
            args[t].min_len = min_len;
            args[t].max_len = max_len;

            start += cnt;
            pthread_create(&threads[t], NULL, find_candidates_thread, &args[t]);
        }

        candmap_init(&full, 256);

        for(int t = 0; t < nthreads; t++) {
            pthread_join(threads[t], NULL);

            candmap_merge_from(&full, &args[t].local);
            candmap_free(&args[t].local);
        }
    }

    CandMap filtered;
    candmap_init(&filtered, full.n > 0 ? full.n : 64);

    for(int i = 0; i < full.n; i++) {
        if(full.entries[i].count >= 2) candmap_incr(&filtered, full.entries[i].key, full.entries[i].keylen, full.entries[i].count);
    }

    candmap_free(&full);
    return filtered;
}

static int seq_matches_pattern_at(const Seq *seq, const int pos, const uint8_t *pat, const int m) {
    if(pos + m > seq->len) return 0;

    for(int k = 0; k < m; k++) {
        if(seq->items[pos + k].type != RAW) return 0;
        if((uint8_t)seq->items[pos + k].val != pat[k]) return 0;
    }

    return 1;
}

static int64_t count_non_overlapping(const Seq *seq, const uint8_t *pat, const int m) {
    int64_t c = 0;
    int i = 0;
    const int limit = seq->len - m;

    while(i <= limit) {
        if(seq_matches_pattern_at(seq, i, pat, m)) {
            c++;
            i += m;
        } else {
            i++;
        }
    }

    return c;
}

static int64_t total_non_overlapping(const SeqList *sl, const uint8_t *pat, const int m) {
    int64_t total = 0;
    for(int i = 0; i < sl->n; i++) total += count_non_overlapping(&sl->seqs[i], pat, m);

    return total;
}

static Seq replace_non_overlapping_one(const Seq *seq, const uint8_t *pat, const int m, const int32_t token_id) {
    Seq out; seq_init(&out, seq->len);
    int i = 0;
    const int limit = seq->len - m;

    while(i < seq->len) {
        if(i <= limit && seq_matches_pattern_at(seq, i, pat, m)) {
            seq_push(&out, TOK, token_id);
            i += m;
        } else {
            seq_push(&out, seq->items[i].type, seq->items[i].val);
            i += 1;
        }
    }

    return out;
}

static SeqList replace_non_overlapping(const SeqList *sl, const uint8_t *pat, const int m, const int32_t token_id) {
    SeqList out; out.n = sl->n;
    out.seqs = (Seq *)malloc(sizeof(Seq) * out.n);

    for(int i = 0; i < sl->n; i++) out.seqs[i] = replace_non_overlapping_one(&sl->seqs[i], pat, m, token_id);

    return out;
}

static int64_t *count_tok_freqs(const SeqList *sl, const int D) {
    int64_t *out = calloc(D > 0 ? D : 1, sizeof(int64_t));

    for(int i = 0; i < sl->n; i++) {
        const Seq *seq = &sl->seqs[i];

        for(int k = 0; k < seq->len; k++) {
            if(seq->items[k].type == TOK) out[seq->items[k].val] += 1;
        }
    }

    return out;
}

static int64_t score_dictionary_bits(const Dictionary *dict, const SeqList *sl, const int *char_bit_len_by_byte, const int encoding) {
    const int D = dict->n;

    int64_t *tok_freqs = count_tok_freqs(sl, D);
    int *tok_bits = codec_token_lengths(encoding, tok_freqs, D);

    int64_t dict_bits = codec_overhead_bits(encoding, D);
    for(int i = 0; i < D; i++) {
        const int64_t entry_header_bits = (int64_t)ref_enc_size_bits((int64_t)dict->entries[i].len);
        int64_t entry_body_bits = 0;

        for(int k = 0; k < dict->entries[i].len; k++) entry_body_bits += char_bit_len_by_byte[dict->entries[i].data[k]];

        dict_bits += entry_header_bits + entry_body_bits;
    }

    int64_t stream_bits = 0;
    for(int i = 0; i < sl->n; i++) {
        const Seq *seq = &sl->seqs[i];
        const int64_t seq_header_bits = (int64_t)ref_enc_size_bits((int64_t)seq->len);

        int64_t seq_body_bits = 0;
        for(int k = 0; k < seq->len; k++) {
            seq_body_bits += 1;

            if(seq->items[k].type == RAW) seq_body_bits += char_bit_len_by_byte[(uint8_t)seq->items[k].val];
            else seq_body_bits += tok_bits[seq->items[k].val];
        }

        stream_bits += seq_header_bits + seq_body_bits;
    }

    free(tok_freqs);
    free(tok_bits);

    return dict_bits + stream_bits;
}

static int token_bits_for_candidate(const int encoding, const int64_t *tok_freqs_raw, const int D_current, const int64_t occ) {
    const int newD = D_current + 1;
    int64_t *combined = malloc(sizeof(int64_t) * newD);

    for(int i = 0; i < D_current; i++) combined[i] = tok_freqs_raw[i];

    combined[D_current] = occ;

    int *lengths = codec_token_lengths(encoding, combined, newD);
    const int result = lengths[D_current];

    free(combined);
    free(lengths);

    return result;
}

static int64_t scoring_function(const uint8_t *pat, const int L, const int64_t occ, const int *char_bit_len_by_byte, const int64_t token_bits_after) {
    int64_t pat_bits = 0;
    for(int i = 0; i < L; i++) pat_bits += char_bit_len_by_byte[pat[i]];

    const int64_t old_cost = occ * (L + pat_bits);
    const int64_t new_cost = occ * (1 + token_bits_after);
    const int64_t dict_cost = (int64_t)ref_enc_size_bits((int64_t)L) + pat_bits;

    return old_cost - new_cost - dict_cost;
}

typedef struct {
    int64_t occ;
    int64_t gain;
    int valid;
} CandScore;

typedef struct {
    const CandMap *cm;
    const SeqList *sl;
    const int64_t *tok_freqs_raw;
    int D;
    int encoding;
    const int *char_bit_len_by_byte;
    CandScore *out;
    int idx_from, idx_to;
    int fixed_token_bits_after;
} ScoreArg;

static void *score_candidates_thread(void *argp) {
    const ScoreArg *a = argp;

    for(int i = a->idx_from; i < a->idx_to; i++) {
        const CandEntry *e = &a->cm->entries[i];
        const int64_t occ = total_non_overlapping(a->sl, e->key, e->keylen);

        if(occ < 2) {
            a->out[i].valid = 0;
            continue;
        }

        int64_t token_bits_after;
        if(a->fixed_token_bits_after >= 0) {
            token_bits_after = a->fixed_token_bits_after;
        } else {
            token_bits_after = token_bits_for_candidate(a->encoding, a->tok_freqs_raw, a->D, occ);
        }

        const int64_t gain = scoring_function(e->key, e->keylen, occ, a->char_bit_len_by_byte, token_bits_after);

        a->out[i].occ = occ;
        a->out[i].gain = gain;
        a->out[i].valid = 1;
    }

    return NULL;
}

static void score_candidates(const CandMap *cm, const SeqList *sl, const int64_t *tok_freqs_raw, const int D, const int encoding, const int *char_bit_len_by_byte, const int fixed_token_bits_after, CandScore *out) {
    if(cm->n == 0) return;

    int nthreads = g_nthreads;
    if(nthreads > cm->n) nthreads = cm->n;
    if(nthreads < 1) nthreads = 1;

    if(nthreads <= 1 || cm->n < 32) {
        ScoreArg a = { .cm = cm, .sl = sl, .tok_freqs_raw = tok_freqs_raw, .D = D, .encoding = encoding, .char_bit_len_by_byte = char_bit_len_by_byte, .out = out, .idx_from = 0, .idx_to = cm->n, .fixed_token_bits_after = fixed_token_bits_after };
        score_candidates_thread(&a);

        return;
    }

    pthread_t threads[nthreads];
    ScoreArg args[nthreads];
    const int base = cm->n / nthreads, rem = cm->n % nthreads;
    int start = 0;

    for(int t = 0; t < nthreads; t++) {
        const int cnt = base + (t < rem ? 1 : 0);

        args[t] = (ScoreArg){ .cm = cm, .sl = sl, .tok_freqs_raw = tok_freqs_raw, .D = D, .encoding = encoding, .char_bit_len_by_byte = char_bit_len_by_byte, .out = out, .idx_from = start, .idx_to = start + cnt, .fixed_token_bits_after = fixed_token_bits_after };
        start += cnt;

        pthread_create(&threads[t], NULL, score_candidates_thread, &args[t]);
    }

    for(int t = 0; t < nthreads; t++) pthread_join(threads[t], NULL);
}

/* ============================================================
 * greedy_build
 * ============================================================ */
static void greedy_build(const StrItem *strs, const int nstrs, const int *char_bit_len_by_byte, const int encoding, const int min_len, const int max_len, const int max_dict, const Dictionary *init_dict, const SeqList *init_seqs, Dictionary *out_dict, SeqList *out_seqs) {
    SeqList seqs = init_seqs ? seqlist_clone(init_seqs) : initial_sequences(strs, nstrs);
    Dictionary dictionary;

    if(init_dict) dictionary = dict_clone(init_dict);
    else dict_init(&dictionary, 4);

    int64_t current_bits = score_dictionary_bits(&dictionary, &seqs, char_bit_len_by_byte, encoding);

    while(dictionary.n < max_dict) {
        const int D = dictionary.n;
        int64_t *tok_freqs = count_tok_freqs(&seqs, D);
        CandMap candidates = find_candidates(&seqs, min_len, max_len);

        CandScore *scores = calloc(candidates.n > 0 ? candidates.n : 1, sizeof(CandScore));
        score_candidates(&candidates, &seqs, tok_freqs, D, encoding, char_bit_len_by_byte, -1, scores);

        int best_idx = -1;
        int64_t best_gain = 0;
        for(int i = 0; i < candidates.n; i++) {
            if(!scores[i].valid) continue;

            if(scores[i].gain > best_gain) {
                best_gain = scores[i].gain;
                best_idx = i;
            }
        }

        free(tok_freqs);
        free(scores);

        if(best_idx < 0 || best_gain <= 0) {
            candmap_free(&candidates);
            break;
        }

        const uint8_t *best_pat = candidates.entries[best_idx].key;
        const int best_len = candidates.entries[best_idx].keylen;

        Dictionary trial_dict = dict_clone(&dictionary);
        dict_push(&trial_dict, best_pat, best_len);
        SeqList trial_seqs = replace_non_overlapping(&seqs, best_pat, best_len, D);

        candmap_free(&candidates);

        const int64_t trial_bits = score_dictionary_bits(&trial_dict, &trial_seqs, char_bit_len_by_byte, encoding);

        if(trial_bits >= current_bits) {
            dict_free(&trial_dict);
            seqlist_free(&trial_seqs);

            break;
        }

        dict_free(&dictionary);
        seqlist_free(&seqs);
        dictionary = trial_dict;
        seqs = trial_seqs;
        current_bits = trial_bits;
    }

    *out_dict = dictionary;
    *out_seqs = seqs;
}

/* ============================================================
 * DFS Branch & Bound
 * ============================================================ */
typedef struct { uint8_t *data; size_t len; size_t cap; } GrowBuf;

static void gb_init(GrowBuf *g, const size_t cap) {
    g->cap = cap > 0 ? cap : 256;
    g->data = (uint8_t *)malloc(g->cap);
    g->len = 0;
}
static void gb_ensure(GrowBuf *g, const size_t extra) {
    if(g->len + extra > g->cap) {
        while(g->len + extra > g->cap) g->cap *= 2;
        g->data = (uint8_t *)realloc(g->data, g->cap);
    }
}
static void gb_push_bytes(GrowBuf *g, const uint8_t *b, const size_t n) {
    gb_ensure(g, n);
    memcpy(g->data + g->len, b, n);
    g->len += n;
}
static void gb_push_uvarint(GrowBuf *g, const uint64_t x) {
    uint8_t tmp[16];
    const size_t n = uvarint_encode(x, tmp);

    gb_push_bytes(g, tmp, n);
}
static void gb_push_byte(GrowBuf *g, const uint8_t b) {
    gb_ensure(g, 1);
    g->data[g->len++] = b;
}

static void serialize_state_key(const Dictionary *dct, const SeqList *sqs, const int depth, GrowBuf *out) {
    gb_init(out, 1024);
    gb_push_uvarint(out, (uint64_t)depth);
    gb_push_uvarint(out, (uint64_t)dct->n);

    for(int i = 0; i < dct->n; i++) {
        gb_push_uvarint(out, (uint64_t)dct->entries[i].len);
        gb_push_bytes(out, dct->entries[i].data, (size_t)dct->entries[i].len);
    }

    gb_push_uvarint(out, (uint64_t)sqs->n);
    for(int i = 0; i < sqs->n; i++) {
        const Seq *seq = &sqs->seqs[i];
        gb_push_uvarint(out, (uint64_t)seq->len);

        for(int k = 0; k < seq->len; k++) {
            gb_push_byte(out, seq->items[k].type);
            gb_push_uvarint(out, (uint64_t)seq->items[k].val);
        }
    }
}

typedef struct {
    uint8_t *key; size_t keylen;
    int64_t bits;
    Dictionary dict;
    SeqList seqs;
    int used;
} MemoEntry;

typedef struct {
    MemoEntry *entries;
    size_t cap;
    size_t count;
} MemoTable;

static void memo_init(MemoTable *m, const size_t cap) {
    m->cap = cap > 0 ? cap : 1024;
    m->entries = (MemoEntry *)calloc(m->cap, sizeof(MemoEntry));
    m->count = 0;
}

static void memo_rehash(MemoTable *m, const size_t newcap) {
    MemoEntry *newentries = calloc(newcap, sizeof(MemoEntry));

    for(size_t i = 0; i < m->cap; i++) {
        if(!m->entries[i].used) continue;

        const uint64_t h = fnv1a(m->entries[i].key, (int)m->entries[i].keylen);
        size_t idx = h % newcap;

        while(newentries[idx].used) idx = (idx + 1) % newcap;

        newentries[idx] = m->entries[i];
    }

    free(m->entries);

    m->entries = newentries;
    m->cap = newcap;
}

static MemoEntry *memo_find(const MemoTable *m, const uint8_t *key, const size_t keylen) {
    const uint64_t h = fnv1a(key, (int)keylen);
    size_t idx = h % m->cap;
    const size_t start = idx;

    while(m->entries[idx].used) {
        if(m->entries[idx].keylen == keylen && memcmp(m->entries[idx].key, key, keylen) == 0) return &m->entries[idx];

        idx = (idx + 1) % m->cap;
        if(idx == start) break;
    }

    return NULL;
}

static void memo_insert(MemoTable *m, const uint8_t *key, const size_t keylen, const int64_t bits, const Dictionary *dict, const SeqList *seqs) {
    if(m->count * 2 >= m->cap) memo_rehash(m, m->cap * 2);

    const uint64_t h = fnv1a(key, (int)keylen);
    size_t idx = h % m->cap;

    while(m->entries[idx].used) idx = (idx + 1) % m->cap;
    m->entries[idx].key = (uint8_t *)malloc(keylen > 0 ? keylen : 1);

    memcpy(m->entries[idx].key, key, keylen);

    m->entries[idx].keylen = keylen;
    m->entries[idx].bits = bits;
    m->entries[idx].dict = dict_clone(dict);
    m->entries[idx].seqs = seqlist_clone(seqs);
    m->entries[idx].used = 1;
    m->count++;
}

static void memo_free(MemoTable *m) {
    for(size_t i = 0; i < m->cap; i++) {
        if(m->entries[i].used) {
            free(m->entries[i].key);
            dict_free(&m->entries[i].dict);
            seqlist_free(&m->entries[i].seqs);
        }
    }

    free(m->entries);

    m->entries = NULL;
    m->cap = 0;
    m->count = 0;
}

typedef struct {
    const StrItem *strs;
    int nstrs;
    const int *char_bit_len_by_byte;
    int encoding;
    int min_len, max_len, max_dict;
    int max_depth;
    int max_depth_is_none;
    MemoTable memo;
    int64_t nodes, pruned_bb, memo_hits;
} DfsCtx;

typedef struct { int64_t bits; Dictionary dict; SeqList seqs; } DfsResult;

static double compute_ub_gain(const DfsCtx *ctx, const CandMap *candidates, const SeqList *sqs) {
    if(candidates->n == 0) return 0.0;

    CandScore *scores = calloc(candidates->n, sizeof(CandScore));
    score_candidates(candidates, sqs, NULL, 0, ctx->encoding, ctx->char_bit_len_by_byte, 1, scores);

    double total = 0.0;
    for(int i = 0; i < candidates->n; i++) {
        if(!scores[i].valid) continue;
        if(scores[i].gain > 0) total += (double)scores[i].gain;
    }

    free(scores);
    return total;
}

typedef struct { int64_t gain; int cand_idx; } ScoredItem;

static int scoreditem_cmp_desc(const void *a, const void *b) {
    const ScoredItem *x = a, *y = b;

    if(x->gain != y->gain) return x->gain > y->gain ? -1 : 1;
    return x->cand_idx - y->cand_idx;
}

static DfsResult dfs_run(DfsCtx *ctx, const Dictionary *dct, const SeqList *sqs, const int64_t current_bits, const int depth) {
    ctx->nodes++;

    GrowBuf key;
    serialize_state_key(dct, sqs, depth, &key);

    const MemoEntry *hit = memo_find(&ctx->memo, key.data, key.len);
    if(hit) {
        ctx->memo_hits++;
        DfsResult r;
        r.bits = hit->bits;
        r.dict = dict_clone(&hit->dict);
        r.seqs = seqlist_clone(&hit->seqs);

        free(key.data);
        return r;
    }

    Dictionary base_dct; SeqList base_sqs;
    greedy_build(ctx->strs, ctx->nstrs, ctx->char_bit_len_by_byte, ctx->encoding, ctx->min_len, ctx->max_len, ctx->max_dict, dct, sqs, &base_dct, &base_sqs);
    const int64_t base_bits = score_dictionary_bits(&base_dct, &base_sqs, ctx->char_bit_len_by_byte, ctx->encoding);

    DfsResult best_local;
    best_local.bits = base_bits;
    best_local.dict = base_dct;
    best_local.seqs = base_sqs;

    if(dct->n >= ctx->max_dict) {
        memo_insert(&ctx->memo, key.data, key.len, best_local.bits, &best_local.dict, &best_local.seqs);
        free(key.data);

        return best_local;
    }

    if(!ctx->max_depth_is_none && depth >= ctx->max_depth) {
        memo_insert(&ctx->memo, key.data, key.len, best_local.bits, &best_local.dict, &best_local.seqs);
        free(key.data);

        return best_local;
    }

    CandMap candidates = find_candidates(sqs, ctx->min_len, ctx->max_len);

    if(candidates.n == 0) {
        candmap_free(&candidates);
        memo_insert(&ctx->memo, key.data, key.len, best_local.bits, &best_local.dict, &best_local.seqs);
        free(key.data);

        return best_local;
    }

    const double ub_gain_bits = compute_ub_gain(ctx, &candidates, sqs);

    if((double)current_bits - ub_gain_bits >= (double)base_bits) {
        ctx->pruned_bb++;

        candmap_free(&candidates);
        memo_insert(&ctx->memo, key.data, key.len, best_local.bits, &best_local.dict, &best_local.seqs);
        free(key.data);

        return best_local;
    }

    const int D = dct->n;
    int64_t *tok_freqs = count_tok_freqs(sqs, D);

    CandScore *scores = calloc(candidates.n, sizeof(CandScore));
    score_candidates(&candidates, sqs, tok_freqs, D, ctx->encoding, ctx->char_bit_len_by_byte, -1, scores);
    free(tok_freqs);

    ScoredItem *scored = malloc(sizeof(ScoredItem) * candidates.n);
    int nscored = 0;

    for(int i = 0; i < candidates.n; i++) {
        if(scores[i].valid && scores[i].gain > 0) {
            scored[nscored].gain = scores[i].gain;
            scored[nscored].cand_idx = i;

            nscored++;
        }
    }

    free(scores);

    if(nscored == 0) {
        free(scored);

        candmap_free(&candidates);
        memo_insert(&ctx->memo, key.data, key.len, best_local.bits, &best_local.dict, &best_local.seqs);

        free(key.data);
        return best_local;
    }

    qsort(scored, nscored, sizeof(ScoredItem), scoreditem_cmp_desc);

    for(int si = 0; si < nscored; si++) {
        const int cidx = scored[si].cand_idx;
        const uint8_t *pat = candidates.entries[cidx].key;
        const int patlen = candidates.entries[cidx].keylen;

        Dictionary new_dct = dict_clone(dct);
        dict_push(&new_dct, pat, patlen);
        SeqList new_sqs = replace_non_overlapping(sqs, pat, patlen, D);

        const int64_t new_bits = score_dictionary_bits(&new_dct, &new_sqs, ctx->char_bit_len_by_byte, ctx->encoding);

        DfsResult cand_result = dfs_run(ctx, &new_dct, &new_sqs, new_bits, depth + 1);

        dict_free(&new_dct);
        seqlist_free(&new_sqs);

        if(cand_result.bits < best_local.bits) {
            dict_free(&best_local.dict);
            seqlist_free(&best_local.seqs);
            best_local = cand_result;
        } else {
            dict_free(&cand_result.dict);
            seqlist_free(&cand_result.seqs);
        }
    }

    free(scored);
    candmap_free(&candidates);

    memo_insert(&ctx->memo, key.data, key.len, best_local.bits, &best_local.dict, &best_local.seqs);
    free(key.data);
    return best_local;
}

static void exhaustive_build(const StrItem *strs, const int nstrs, const int *char_bit_len_by_byte, const int encoding, const int min_len, const int max_len, const int max_dict, const int max_depth, const int max_depth_is_none, Dictionary *out_dict, SeqList *out_seqs) {
    if(!max_depth_is_none && max_depth == 0) {
        greedy_build(strs, nstrs, char_bit_len_by_byte, encoding, min_len, max_len, max_dict, NULL, NULL, out_dict, out_seqs);
        return;
    }

    SeqList init_seqs = initial_sequences(strs, nstrs);

    DfsCtx ctx;

    ctx.strs = strs; ctx.nstrs = nstrs;
    ctx.char_bit_len_by_byte = char_bit_len_by_byte;
    ctx.encoding = encoding;
    ctx.min_len = min_len; ctx.max_len = max_len; ctx.max_dict = max_dict;
    ctx.max_depth = max_depth; ctx.max_depth_is_none = max_depth_is_none;

    memo_init(&ctx.memo, 1024);
    ctx.nodes = ctx.pruned_bb = ctx.memo_hits = 0;

    Dictionary empty_dict; dict_init(&empty_dict, 4);
    const int64_t init_bits = score_dictionary_bits(&empty_dict, &init_seqs, char_bit_len_by_byte, encoding);

    const DfsResult result = dfs_run(&ctx, &empty_dict, &init_seqs, init_bits, 0);

    dict_free(&empty_dict);
    seqlist_free(&init_seqs);
    memo_free(&ctx.memo);

    log_line("DFS: nodes = %lld, pruned = %lld, memo_hits = %lld", (long long)ctx.nodes, (long long)ctx.pruned_bb, (long long)ctx.memo_hits);

    *out_dict = result.dict;
    *out_seqs = result.seqs;
}

typedef struct { int idx; int64_t f; } RankItem;

static int rankitem_cmp_desc_stable(const void *a, const void *b) {
    const RankItem *x = a, *y = b;

    if(x->f != y->f) return x->f > y->f ? -1 : 1;
    return x->idx - y->idx;
}

static void reorder_dict_for_positional(Dictionary *dict, SeqList *seqs, int64_t **out_tok_freqs) {
    const int D = dict->n;
    int64_t *tok_freqs = count_tok_freqs(seqs, D);

    RankItem *ri = malloc(sizeof(RankItem) * (D > 0 ? D : 1));
    for(int i = 0; i < D; i++) {
        ri[i].idx = i;
        ri[i].f = tok_freqs[i];
    }
    qsort(ri, D, sizeof(RankItem), rankitem_cmp_desc_stable);

    Dictionary new_dict; dict_init(&new_dict, D > 0 ? D : 4);
    int *old_to_new = malloc(sizeof(int) * (D > 0 ? D : 1));
    for(int newpos = 0; newpos < D; newpos++) {
        const int old = ri[newpos].idx;
        dict_push(&new_dict, dict->entries[old].data, dict->entries[old].len);
        old_to_new[old] = newpos;
    }

    SeqList new_seqs; new_seqs.n = seqs->n;
    new_seqs.seqs = (Seq *)malloc(sizeof(Seq) * new_seqs.n);
    for(int i = 0; i < seqs->n; i++) {
        const Seq *src = &seqs->seqs[i];
        Seq dst; seq_init(&dst, src->len);

        for(int k = 0; k < src->len; k++) {
            if(src->items[k].type == TOK) seq_push(&dst, TOK, old_to_new[src->items[k].val]);
            else seq_push(&dst, RAW, src->items[k].val);
        }

        new_seqs.seqs[i] = dst;
    }

    dict_free(dict);
    seqlist_free(seqs);
    *dict = new_dict;
    *seqs = new_seqs;

    free(ri);
    free(old_to_new);
    free(tok_freqs);

    *out_tok_freqs = count_tok_freqs(seqs, D);
}

/* ============================================================
 * Sections
 * ============================================================ */
static void write_alphabet_section(BitWriter *bw, const Alphabet *alph, const int encoding) {
    const int A = alph->A;
    bw_write_uvarint_aligned(bw, (uint64_t)A);
    bw_write_bytes_aligned(bw, alph->alphabet, (size_t)A);

    int64_t char_freq_by_id[256];
    alphabet_char_freq_by_id(alph, char_freq_by_id);
    codec_write_overhead(encoding, bw, char_freq_by_id, A);
}

typedef struct {
    uint8_t alphabet[256];
    int A;
    Decoder decoder;
} ReadAlphabetResult;

static ReadAlphabetResult read_alphabet_section(const uint8_t *data, size_t *pos, const int encoding) {
    ReadAlphabetResult r = {0};
    const uint64_t A64 = uvarint_decode(data, pos);
    int A = (int)A64;

    memcpy(r.alphabet, data + *pos, (size_t)A);
    *pos += (size_t)A;

    if(A == 0) {
        r.alphabet[0] = 0;
        A = 1;
    }

    BitReader br_tmp;
    br_init(&br_tmp, data, SIZE_MAX, *pos);
    int *lengths = codec_read_overhead(encoding, &br_tmp, A);
    *pos = br_tmp.pos;

    r.A = A;
    r.decoder = codec_decoder_from_lengths(encoding, lengths, A);
    free(lengths);

    return r;
}

static void write_dictionary_section(BitWriter *bw, const Dictionary *dict, const int *byte_to_id, const int64_t *tok_freqs_raw, const HCode *char_codes, const int char_bits, const int encoding) {
    const int D = dict->n;
    bw_write_uvarint_aligned(bw, (uint64_t)D);

    for (int i = 0; i < D; i++) {
        bw_write_uvarint_aligned(bw, (uint64_t)dict->entries[i].len);

        for(int k = 0; k < dict->entries[i].len; k++) {
            const uint8_t b = dict->entries[i].data[k];
            codec_write_symbol(encoding, bw, byte_to_id[b], char_codes, char_bits);
        }
    }

    codec_write_overhead(encoding, bw, tok_freqs_raw, D);
}

static Dictionary read_dictionary_section(BitReader *br, const uint8_t *alphabet, const Decoder *char_decoder, const int encoding, const int num_entries, Decoder *out_tok_decoder) {
    Dictionary dict; dict_init(&dict, num_entries > 0 ? num_entries : 4);

    uint8_t tmpbuf[4096];
    for(int e = 0; e < num_entries; e++) {
        br_align_to_byte(br);
        const uint64_t L = br_read_uvarint_aligned(br);

        uint8_t *entry = L <= sizeof(tmpbuf) ? tmpbuf : malloc(L);
        for(uint64_t k = 0; k < L; k++) {
            const int cid = codec_read_symbol(encoding, br, char_decoder);
            entry[k] = alphabet[cid];
        }

        dict_push(&dict, entry, (int)L);

        if(entry != tmpbuf) free(entry);
    }

    br_align_to_byte(br);
    int *lengths = codec_read_overhead(encoding, br, num_entries);
    *out_tok_decoder = codec_decoder_from_lengths(encoding, lengths, num_entries);
    free(lengths);

    return dict;
}

static void write_stream(BitWriter *bw, const SeqList *seqs, const int *byte_to_id, const HCode *char_codes, const int char_bits, const HCode *tok_codes, const int token_bits, const int encoding) {
    bw_write_uvarint_aligned(bw, (uint64_t)seqs->n);

    for(int i = 0; i < seqs->n; i++) {
        const Seq *seq = &seqs->seqs[i];
        bw_write_uvarint_aligned(bw, (uint64_t)seq->len);

        for(int k = 0; k < seq->len; k++) {
            if(seq->items[k].type == RAW) {
                bw_write_bits(bw, 0, 1);
                codec_write_symbol(encoding, bw, byte_to_id[(uint8_t)seq->items[k].val], char_codes, char_bits);
            } else {
                bw_write_bits(bw, 1, 1);
                codec_write_symbol(encoding, bw, seq->items[k].val, tok_codes, token_bits);
            }
        }
    }
}

static StrItem *read_stream(BitReader *br, const uint8_t *alphabet, const Dictionary *dict, const int encoding, const Decoder *char_decoder, const Decoder *tok_decoder, int *out_n) {
    br_align_to_byte(br);
    const uint64_t N = br_read_uvarint_aligned(br);

    StrItem *out = malloc(sizeof(StrItem) * (N > 0 ? N : 1));

    for(uint64_t i = 0; i < N; i++) {
        br_align_to_byte(br);
        const uint64_t S = br_read_uvarint_aligned(br);

        ByteBuf buf; bytebuf_init(&buf, (S > 0 ? S : 1) * 4 + 16);

        for(uint64_t k = 0; k < S; k++) {
            const uint64_t flag = br_read_bits(br, 1);

            if(flag == 0) {
                const int cid = codec_read_symbol(encoding, br, char_decoder);
                bytebuf_push(&buf, alphabet[cid]);
            } else {
                const int tid = codec_read_symbol(encoding, br, tok_decoder);
                const DictEntry *de = &dict->entries[tid];

                for(int b = 0; b < de->len; b++) bytebuf_push(&buf, de->data[b]);
            }
        }

        out[i].data = buf.data;
        out[i].len = buf.len;
    }

    *out_n = (int)N;
    return out;
}

/* ============================================================
 * Encode / Decode
 * ============================================================ */
static double now_seconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int freq_init = 0;

    if(!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void encode_onefile(const char *project_root, const char *input_txt, const char *output_bin, const int min_len, const int max_len, const int max_dict, const int exh_max_depth, const int encoding) {
    const double t_start = now_seconds();

    char inputs_dir[4096];
    snprintf(inputs_dir, sizeof(inputs_dir), "%s/inputs", project_root);

    int nstrs = 0;
    StrItem *strs = read_strings_text(inputs_dir, input_txt, &nstrs);

    const int sort_by_freq = (encoding == ENC_POSITIONAL);
    Alphabet alph;
    build_alphabet(strs, nstrs, sort_by_freq, &alph);

    int64_t char_freq_by_id[256];
    alphabet_char_freq_by_id(&alph, char_freq_by_id);

    int *char_lengths_by_id0 = codec_char_lengths(encoding, alph.A, char_freq_by_id);
    int char_bit_len_by_byte[256] = {0};
    compute_char_bit_lengths(alph.alphabet, alph.A, char_lengths_by_id0, char_bit_len_by_byte);
    free(char_lengths_by_id0);

    const int max_depth_is_none = (exh_max_depth < 0);
    const int max_depth = max_depth_is_none ? 0 : exh_max_depth;

    Dictionary dictionary; SeqList seqs;
    exhaustive_build(strs, nstrs, char_bit_len_by_byte, encoding, min_len, max_len, max_dict, max_depth, max_depth_is_none, &dictionary, &seqs);

    const int D = dictionary.n;
    const int token_bits = needed_bits(D);

    int64_t *tok_freqs;
    if(encoding == ENC_POSITIONAL) reorder_dict_for_positional(&dictionary, &seqs, &tok_freqs);
    else tok_freqs = count_tok_freqs(&seqs, D);

    int *char_lengths_by_id = codec_char_lengths(encoding, alph.A, char_freq_by_id);
    HCode *char_codes = codec_encode_codes_from_lengths(encoding, char_lengths_by_id, alph.A);

    int *tok_lengths_by_id = codec_token_lengths(encoding, tok_freqs, D);
    HCode *tok_codes = codec_encode_codes_from_lengths(encoding, tok_lengths_by_id, D);

    BitWriter bw;
    bw_init(&bw);
    bw_write_bytes_aligned(&bw, MAGIC, 4);
    const uint8_t verenc[2] = { (uint8_t)VERSION, (uint8_t)encoding };
    bw_write_bytes_aligned(&bw, verenc, 2);

    write_alphabet_section(&bw, &alph, encoding);
    write_dictionary_section(&bw, &dictionary, alph.byte_to_id, tok_freqs, char_codes, alph.char_bits, encoding);
    write_stream(&bw, &seqs, alph.byte_to_id, char_codes, alph.char_bits, tok_codes, token_bits, encoding);

    size_t datalen;
    uint8_t *data = bw_getvalue(&bw, &datalen);

    char output_dir[4096];
    snprintf(output_dir, sizeof(output_dir), "%s/outputs", project_root);
    mkdirs(output_dir);

    char full_output_path[4096];
    snprintf(full_output_path, sizeof(full_output_path), "%s/%s", output_dir, output_bin);

    FILE *f = fopen(full_output_path, "wb");
    if(!f) {
        fprintf(stderr, "Error: unable to write %s: %s\n", full_output_path, strerror(errno));
        exit(1);
    }

    fwrite(data, 1, datalen, f);
    fclose(f);

    int64_t orig_bytes = 0;
    for(int i = 0; i < nstrs; i++) orig_bytes += (int64_t)strs[i].len;
    int64_t dict_bytes = 0;
    for(int i = 0; i < dictionary.n; i++) dict_bytes += dictionary.entries[i].len;

    const double t_elapsed = now_seconds() - t_start;

    log_line("OK: scritto %s", output_bin);
    log_line("Stringhe: %d", nstrs);
    log_line("Originale (UTF-8 bytes): %lld", (long long)orig_bytes);
    log_line("Alfabeto A = %d => char_bits = %d", alph.A, alph.char_bits);
    log_line("Dizionario D = %d => token_bits = %d, bytes_dizionario_raw = %lld", D, token_bits, (long long)dict_bytes);
    log_line("Output totale (bytes): %zu", datalen);
    log_line("Tempo: %.2f s", t_elapsed);

    free(data);
    free(char_lengths_by_id);
    free(char_codes);
    free(tok_lengths_by_id);
    free(tok_codes);
    free(tok_freqs);

    dict_free(&dictionary);
    seqlist_free(&seqs);
    for(int i = 0; i < nstrs; i++) free(strs[i].data);

    free(strs);
}

static StrItem *decompress_onefile(const char *project_root, const char *path_bin, int *out_n) {
    char output_dir[4096];
    snprintf(output_dir, sizeof(output_dir), "%s/outputs", project_root);

    char full_path[4096];
    snprintf(full_path, sizeof(full_path), "%s/%s", output_dir, path_bin);

    FILE *f = fopen(full_path, "rb");
    if(!f) {
        fprintf(stderr, "Error: unable to open %s: %s\n", full_path, strerror(errno));
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);

    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)fsize);

    if(fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "Error: unable to open %s\n", full_path);
        exit(1);
    }

    fclose(f);

    size_t pos = 0;
    if(fsize < 4 || memcmp(data, MAGIC, 4) != 0) {
        fprintf(stderr, "Error: invalid MAGIC\n");
        exit(1);
    }

    pos += 4;
    const int ver = data[pos]; pos += 1;
    const int encoding = data[pos]; pos += 1;

    if(ver != VERSION) {
        fprintf(stderr, "Error: unsupported version: %d\n", ver);
        exit(1);
    }

    ReadAlphabetResult ar = read_alphabet_section(data, &pos, encoding);

    const uint64_t D = uvarint_decode(data, &pos);

    BitReader br;
    br_init(&br, data, (size_t)fsize, pos);

    Decoder tok_decoder;
    Dictionary dictionary = read_dictionary_section(&br, ar.alphabet, &ar.decoder, encoding, (int)D, &tok_decoder);

    int n;
    StrItem *result = read_stream(&br, ar.alphabet, &dictionary, encoding, &ar.decoder, &tok_decoder, &n);

    decoder_free(&ar.decoder);
    decoder_free(&tok_decoder);
    dict_free(&dictionary);
    free(data);

    *out_n = n;
    return result;
}

/* ============================================================
 * Library API (for ctypes binding from Python)
 *
 * Wraps the dictionary-building phase only (alphabet + greedy/DFS
 * search): the expensive O(candidates^2)-ish part. Bit-level
 * serialization stays in Python's compression.py, which remains the
 * single source of truth for the exact QRtree bitstream layout.
 * ============================================================ */
typedef struct { uint8_t *data; int32_t len; } QBuf;
typedef struct { uint8_t type; int32_t val; } QSym;
typedef struct { QSym *items; int32_t len; } QSeq;

typedef struct {
    uint8_t alphabet[256];
    int32_t A;
    int32_t byte_to_id[256];  /* -1 if the byte is unused */

    QBuf *dict_entries;
    int32_t dict_count;

    QSeq *seqs;
    int32_t seqs_count;
} QRTreeBuildResult;

/*
 * strings/string_lens: n_strings byte buffers (not necessarily
 * NUL-terminated -- lengths are explicit) with the program's literal
 * strings, in order. encoding: 0=fixed,1=huffman-freq,2=huffman-len,
 * 3=positional. exh_max_depth: 0=greedy, -1=unlimited DFS, N=depth N.
 * nthreads<=0 keeps the current setting.
 */
QRTreeBuildResult *qrtree_build(const uint8_t **strings, const int32_t *string_lens, const int32_t n_strings,
                                 const int32_t encoding, const int32_t min_len, const int32_t max_len,
                                 const int32_t max_dict, const int32_t exh_max_depth, const int32_t nthreads) {
    if(nthreads > 0) g_nthreads = nthreads;

    StrItem *strs = (StrItem *)malloc(sizeof(StrItem) * (n_strings > 0 ? n_strings : 1));
    for(int i = 0; i < n_strings; i++) {
        strs[i].data = (uint8_t *)strings[i];
        strs[i].len = (size_t)string_lens[i];
    }

    const int sort_by_freq = (encoding == ENC_POSITIONAL);
    Alphabet alph;
    build_alphabet(strs, n_strings, sort_by_freq, &alph);

    int64_t char_freq_by_id[256];
    alphabet_char_freq_by_id(&alph, char_freq_by_id);

    int *char_lengths_by_id0 = codec_char_lengths(encoding, alph.A, char_freq_by_id);
    int char_bit_len_by_byte[256] = {0};
    compute_char_bit_lengths(alph.alphabet, alph.A, char_lengths_by_id0, char_bit_len_by_byte);
    free(char_lengths_by_id0);

    const int max_depth_is_none = (exh_max_depth < 0);
    const int max_depth = max_depth_is_none ? 0 : exh_max_depth;

    Dictionary dictionary; SeqList seqs;
    exhaustive_build(strs, n_strings, char_bit_len_by_byte, encoding, min_len, max_len, max_dict,
                      max_depth, max_depth_is_none, &dictionary, &seqs);

    if(encoding == ENC_POSITIONAL) {
        int64_t *tok_freqs_tmp;
        reorder_dict_for_positional(&dictionary, &seqs, &tok_freqs_tmp);
        free(tok_freqs_tmp);
    }

    QRTreeBuildResult *out = (QRTreeBuildResult *)malloc(sizeof(QRTreeBuildResult));
    memcpy(out->alphabet, alph.alphabet, 256);
    out->A = alph.A;
    for(int i = 0; i < 256; i++) out->byte_to_id[i] = alph.byte_to_id[i];

    out->dict_count = dictionary.n;
    out->dict_entries = (QBuf *)malloc(sizeof(QBuf) * (dictionary.n > 0 ? dictionary.n : 1));
    for(int i = 0; i < dictionary.n; i++) {
        out->dict_entries[i].len = dictionary.entries[i].len;
        out->dict_entries[i].data = (uint8_t *)malloc((size_t)dictionary.entries[i].len);
        memcpy(out->dict_entries[i].data, dictionary.entries[i].data, (size_t)dictionary.entries[i].len);
    }

    out->seqs_count = seqs.n;
    out->seqs = (QSeq *)malloc(sizeof(QSeq) * (seqs.n > 0 ? seqs.n : 1));
    for(int i = 0; i < seqs.n; i++) {
        out->seqs[i].len = seqs.seqs[i].len;
        out->seqs[i].items = (QSym *)malloc(sizeof(QSym) * (size_t)(seqs.seqs[i].len > 0 ? seqs.seqs[i].len : 1));
        for(int k = 0; k < seqs.seqs[i].len; k++) {
            out->seqs[i].items[k].type = seqs.seqs[i].items[k].type;
            out->seqs[i].items[k].val = seqs.seqs[i].items[k].val;
        }
    }

    dict_free(&dictionary);
    seqlist_free(&seqs);
    free(strs);

    return out;
}

void qrtree_free_result(QRTreeBuildResult *r) {
    if(!r) return;

    for(int i = 0; i < r->dict_count; i++) free(r->dict_entries[i].data);
    free(r->dict_entries);

    for(int i = 0; i < r->seqs_count; i++) free(r->seqs[i].items);
    free(r->seqs);

    free(r);
}

/* ============================================================
 * CLI
 * ============================================================ */
static int encoding_from_name(const char *name) {
    if(strcmp(name, "fixed") == 0) return ENC_FIXED;
    if(strcmp(name, "huffman-freq") == 0) return ENC_HUFF_FREQ;
    if(strcmp(name, "huffman-len") == 0) return ENC_HUFF_LEN;
    if(strcmp(name, "positional") == 0) return ENC_POSITIONAL;

    return -1;
}

static int detect_nthreads(void) {
    long n;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    n = (long)si.dwNumberOfProcessors;
#else
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if(n < 1) n = 1;
    if(n > 64) n = 64;

    return (int)n;
}

int main(const int argc, char **argv) {
    const char *mode = NULL;
    const char *input = NULL;
    const char *output = NULL;
    int min_len = 2, max_len = 32, max_dict = 1023, exh_max_depth = 0;
    const char *encoding_name = "fixed";

    const char *positionals[3];
    int npos = 0;

    for(int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if(strcmp(a, "--min-len") == 0 && i + 1 < argc) {
            min_len = atoi(argv[++i]);
        } else if(strcmp(a, "--max-len") == 0 && i + 1 < argc) {
            max_len = atoi(argv[++i]);
        } else if(strcmp(a, "--max-dict") == 0 && i + 1 < argc) {
            max_dict = atoi(argv[++i]);
        } else if(strcmp(a, "--exh-max-depth") == 0 && i + 1 < argc) {
            exh_max_depth = atoi(argv[++i]);
        } else if(strcmp(a, "--encoding") == 0 && i + 1 < argc) {
            encoding_name = argv[++i];
        } else if(a[0] == '-' && strlen(a) > 1 && !(a[1] >= '0' && a[1] <= '9')) {
            fprintf(stderr, "%s: unknown option: %s\n", argv[0], a);
            return 2;
        } else {
            if(npos < 3) positionals[npos++] = a;
        }
    }

    if(npos < 2) {
        return 2;
    }

    mode = positionals[0];
    input = positionals[1];
    if(npos >= 3) output = positionals[2];

    if(strcmp(mode, "compress") != 0 && strcmp(mode, "decompress") != 0) {
        fprintf(stderr, "%s: invalid mode: %s (choose between 'compress', 'decompress')\n", argv[0], mode);
        return 2;
    }

    const int encoding = encoding_from_name(encoding_name);
    if(encoding < 0) {
        fprintf(stderr, "%s: --encoding invalid: %s\n", argv[0], encoding_name);
        return 2;
    }

    g_nthreads = detect_nthreads();
    setup_logger(input);

    char project_root[4096];
    get_project_root(project_root, sizeof(project_root));

    if(strcmp(mode, "compress") == 0) {
        char out_buf[4096];
        const char *out;

        if(output) {
            out = output;
        } else {
            char base[4096];
            snprintf(base, sizeof(base), "%s", input);
            char *dot = strchr(base, '.');

            if(dot) *dot = '\0';

            snprintf(out_buf, sizeof(out_buf), "c2_exhaustive_c_%s_compressed.bin", base);
            out = out_buf;
        }

        encode_onefile(project_root, input, out, min_len, max_len, max_dict, exh_max_depth, encoding);
    } else {
        int n;
        StrItem *strs = decompress_onefile(project_root, input, &n);

        if(output) {
            char output_dir[4096];
            snprintf(output_dir, sizeof(output_dir), "%s/outputs", project_root);
            mkdirs(output_dir);

            char full_out[4096];
            snprintf(full_out, sizeof(full_out), "%s/%s", output_dir, output);

            FILE *f = fopen(full_out, "w");
            if(!f) {
                fprintf(stderr, "Error: unable to write %s: %s\n", full_out, strerror(errno));
                return 1;
            }

            for(int i = 0; i < n; i++) {
                fwrite(strs[i].data, 1, strs[i].len, f);
                fputc('\n', f);
            }

            fclose(f);

            log_line("OK: scritto %s", output);
        } else {
            for(int i = 0; i < n; i++) {
                char *tmp = malloc(strs[i].len + 1);
                memcpy(tmp, strs[i].data, strs[i].len);
                tmp[strs[i].len] = '\0';

                log_line("%s", tmp);

                free(tmp);
            }
        }

        for(int i = 0; i < n; i++) free(strs[i].data);
        free(strs);
    }

    if(g_logfile) fclose(g_logfile);
    return 0;
}