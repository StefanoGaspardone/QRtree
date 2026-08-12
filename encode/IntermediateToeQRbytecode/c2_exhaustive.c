#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>

/* ============================================================
 * Constants
 * ============================================================ */
#define RAW 0
#define TOK 1

#define ENC_FIXED 0
#define ENC_HUFF_FREQ 1
#define ENC_HUFF_LEN 2
#define ENC_POSITIONAL 3

static int g_nthreads = 1;

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

static int needed_bits(int64_t n) {
    if(n <= 1) return 1;
    return (int)ceil(log2((double)n));
}

static int64_t exponential_ones_value(const int ones) {
    if(ones == 0) return 0;
    if(ones == 4) return (1LL << 4) - 1;
    return exponential_ones_value(ones / 2) + (1LL << (ones / 2)) - 1;
}

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

/* ============================================================
 * Codec length
 * ============================================================ */
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

typedef struct { uint8_t *data; size_t len; } StrItem;

/* ============================================================
 * Sym / Seq / SeqList
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

/* ============================================================
 * Dictionary
 * ============================================================ */
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

/* ============================================================
 * Candidate map
 * ============================================================ */
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

/* ============================================================
 * Thread pool
 * ============================================================ */
typedef void (*PoolFn)(void *);

typedef struct ThreadPool ThreadPool;

typedef struct {
    ThreadPool *pool;
    int id;
} PoolWorkerCtx;

struct ThreadPool {
    int nthreads;
    pthread_t *threads;
    PoolWorkerCtx *worker_ctx;

    pthread_mutex_t mutex;
    pthread_cond_t cond_work;
    pthread_cond_t cond_done;

    PoolFn fn;
    void **args;
    int ntasks;
    int completed;
    uint64_t round;
    int shutdown;
};

static ThreadPool *g_pool = NULL;

static void *pool_worker(void *argp) {
    PoolWorkerCtx *ctx = (PoolWorkerCtx *)argp;
    ThreadPool *p = ctx->pool;
    uint64_t last_round = 0;

    pthread_mutex_lock(&p->mutex);
    for(;;) {
        while(!p->shutdown && p->round == last_round) pthread_cond_wait(&p->cond_work, &p->mutex);

        if(p->shutdown) {
            pthread_mutex_unlock(&p->mutex);
            return NULL;
        }

        last_round = p->round;

        const int has_task = ctx->id < p->ntasks;
        PoolFn fn = p->fn;
        void *arg = has_task ? p->args[ctx->id] : NULL;

        pthread_mutex_unlock(&p->mutex);

        if(has_task) fn(arg);

        pthread_mutex_lock(&p->mutex);
        p->completed++;

        if(p->completed == p->nthreads) pthread_cond_signal(&p->cond_done);
    }
}

static void pool_init(ThreadPool *p, int nthreads) {
    if(nthreads < 1) nthreads = 1;

    p->nthreads = nthreads;
    p->threads = malloc(sizeof(pthread_t) * nthreads);
    p->worker_ctx = malloc(sizeof(PoolWorkerCtx) * nthreads);

    pthread_mutex_init(&p->mutex, NULL);
    pthread_cond_init(&p->cond_work, NULL);
    pthread_cond_init(&p->cond_done, NULL);

    p->fn = NULL;
    p->args = NULL;
    p->ntasks = 0;
    p->completed = 0;
    p->round = 0;
    p->shutdown = 0;

    if(nthreads > 1) {
        for(int i = 0; i < nthreads; i++) {
            p->worker_ctx[i].pool = p;
            p->worker_ctx[i].id = i;

            pthread_create(&p->threads[i], NULL, pool_worker, &p->worker_ctx[i]);
        }
    }
}

static void pool_shutdown(ThreadPool *p) {
    if(p->nthreads > 1) {
        pthread_mutex_lock(&p->mutex);
        p->shutdown = 1;
        pthread_cond_broadcast(&p->cond_work);
        pthread_mutex_unlock(&p->mutex);

        for(int i = 0; i < p->nthreads; i++) pthread_join(p->threads[i], NULL);
    }

    free(p->threads);
    free(p->worker_ctx);

    pthread_mutex_destroy(&p->mutex);
    pthread_cond_destroy(&p->cond_work);
    pthread_cond_destroy(&p->cond_done);

    p->threads = NULL;
    p->worker_ctx = NULL;
}

static void pool_run(ThreadPool *p, PoolFn fn, void **args, const int n) {
    if(p->nthreads <= 1) {
        for(int i = 0; i < n; i++) fn(args[i]);
        return;
    }

    pthread_mutex_lock(&p->mutex);

    p->fn = fn;
    p->args = args;
    p->ntasks = n;
    p->completed = 0;
    p->round++;

    pthread_cond_broadcast(&p->cond_work);

    while(p->completed < p->nthreads) pthread_cond_wait(&p->cond_done, &p->mutex);

    pthread_mutex_unlock(&p->mutex);
}

/* ============================================================
 * find_candidates
 * ============================================================ */
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

static void find_candidates_task(void *arg) {
    FindCandArg *a = arg;

    candmap_init(&a->local, 256);
    find_candidates_range(a->sl, a->seq_from, a->seq_to, a->min_len, a->max_len, &a->local);
}

static CandMap find_candidates(const SeqList *sl, const int min_len, const int max_len) {
    CandMap full;

    int nthreads = g_nthreads;
    if(nthreads > sl->n) nthreads = sl->n > 0 ? sl->n : 1;
    if(nthreads < 1) nthreads = 1;

    if(nthreads <= 1 || sl->n < 32 || !g_pool) {
        candmap_init(&full, 256);
        find_candidates_range(sl, 0, sl->n, min_len, max_len, &full);
    } else {
        FindCandArg args[nthreads];
        void *argp[nthreads];

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
            argp[t] = &args[t];
        }

        pool_run(g_pool, find_candidates_task, argp, nthreads);

        candmap_init(&full, 256);
        for(int t = 0; t < nthreads; t++) {
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

/* ============================================================
 * Pattern matching / replacement over sequences
 * ============================================================ */
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

/* ============================================================
 * Scoring
 * ============================================================ */
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

static void score_candidates_task(void *argp) {
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
}

static void score_candidates(const CandMap *cm, const SeqList *sl, const int64_t *tok_freqs_raw, const int D, const int encoding, const int *char_bit_len_by_byte, const int fixed_token_bits_after, CandScore *out) {
    if(cm->n == 0) return;

    int nthreads = g_nthreads;
    if(nthreads > cm->n) nthreads = cm->n;
    if(nthreads < 1) nthreads = 1;

    if(nthreads <= 1 || cm->n < 32 || !g_pool) {
        ScoreArg a = { .cm = cm, .sl = sl, .tok_freqs_raw = tok_freqs_raw, .D = D, .encoding = encoding, .char_bit_len_by_byte = char_bit_len_by_byte, .out = out, .idx_from = 0, .idx_to = cm->n, .fixed_token_bits_after = fixed_token_bits_after };
        score_candidates_task(&a);

        return;
    }

    ScoreArg args[nthreads];
    void *argp[nthreads];

    const int base = cm->n / nthreads, rem = cm->n % nthreads;
    int start = 0;

    for(int t = 0; t < nthreads; t++) {
        const int cnt = base + (t < rem ? 1 : 0);

        args[t] = (ScoreArg){ .cm = cm, .sl = sl, .tok_freqs_raw = tok_freqs_raw, .D = D, .encoding = encoding, .char_bit_len_by_byte = char_bit_len_by_byte, .out = out, .idx_from = start, .idx_to = start + cnt, .fixed_token_bits_after = fixed_token_bits_after };
        start += cnt;
        argp[t] = &args[t];
    }

    pool_run(g_pool, score_candidates_task, argp, nthreads);
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
    GrowBuf key;
    serialize_state_key(dct, sqs, depth, &key);

    const MemoEntry *hit = memo_find(&ctx->memo, key.data, key.len);
    if(hit) {
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

    Dictionary empty_dict; dict_init(&empty_dict, 4);
    const int64_t init_bits = score_dictionary_bits(&empty_dict, &init_seqs, char_bit_len_by_byte, encoding);

    const DfsResult result = dfs_run(&ctx, &empty_dict, &init_seqs, init_bits, 0);

    dict_free(&empty_dict);
    seqlist_free(&init_seqs);
    memo_free(&ctx.memo);

    *out_dict = result.dict;
    *out_seqs = result.seqs;
}

/* ============================================================
 * Canonical Huffman codes
 * ============================================================ */
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
    char *buf;
    size_t len;
    size_t cap;
} TextBitWriter;

static void tbw_init(TextBitWriter *w, size_t cap) {
    w->cap = cap > 0 ? cap : 1024;
    w->buf = (char *)malloc(w->cap);
    w->len = 0;
}

static void tbw_ensure(TextBitWriter *w, size_t extra) {
    if(w->len + extra > w->cap) {
        while(w->len + extra > w->cap) w->cap *= 2;
        w->buf = (char *)realloc(w->buf, w->cap);
    }
}

static void tbw_push_bits_msb(TextBitWriter *w, const uint64_t value, const int nbits) {
    tbw_ensure(w, (size_t)nbits);

    for(int i = nbits - 1; i >= 0; i--) w->buf[w->len++] = (char)(((value >> i) & 1ULL) ? '1' : '0');
}

static char *tbw_finish(TextBitWriter *w) {
    tbw_ensure(w, 1);
    w->buf[w->len] = '\0';

    return w->buf;
}

static void ref_enc_write(TextBitWriter *w, const int64_t value) {
    int length = 4;

    for(;;) {
        const int ones = (length == 4) ? 0 : length / 2;
        const int width = (length == 4) ? 4 : length / 2;
        const int64_t max_value = (1LL << width) - 1;
        const int64_t base = exponential_ones_value(ones);
        const int64_t cur_value = value - base;

        if(cur_value < max_value) {
            tbw_ensure(w, (size_t)(ones + width));

            for(int i = 0; i < ones; i++) w->buf[w->len++] = '1';
            for(int i = width - 1; i >= 0; i--) w->buf[w->len++] = (char)(((cur_value >> i) & 1) ? '1' : '0');

            return;
        }

        length *= 2;
    }
}

static void elias_write_ascii(TextBitWriter *w, const int64_t i) {
    const uint64_t n = (uint64_t)i + 1;
    const int bl = elias_bitlen(n);
    const int k = bl - 1;

    tbw_push_bits_msb(w, n, 2 * k + 1);
}

static void codec_write_symbol_ascii(const int encoding, TextBitWriter *w, const int sym_id, const HCode *codes, const int fixed_bits) {
    switch(encoding) {
        case ENC_FIXED:
            tbw_push_bits_msb(w, (uint64_t)sym_id, fixed_bits);
            break;
        case ENC_POSITIONAL:
            elias_write_ascii(w, sym_id);
            break;
        case ENC_HUFF_FREQ:
        case ENC_HUFF_LEN:
            tbw_push_bits_msb(w, codes[sym_id].code, codes[sym_id].length);
            break;
    }
}

static void codec_write_overhead_ascii(const int encoding, TextBitWriter *w, const int64_t *freq_raw, const int count) {
    switch(encoding) {
        case ENC_FIXED:
        case ENC_POSITIONAL:
            break;
        case ENC_HUFF_FREQ: {
            int64_t *full = malloc(sizeof(int64_t) * (count > 0 ? count : 1));
            for(int i = 0; i < count; i++) full[i] = freq_raw[i] != 0 ? freq_raw[i] : 1;

            uint8_t *norm = normalize_freqs(full, count);
            for(int i = 0; i < count; i++) tbw_push_bits_msb(w, norm[i], 8);

            free(full); free(norm);
            break;
        }
        case ENC_HUFF_LEN: {
            int *lens = count > 0 ? huffman_len_lengths(freq_raw, count) : NULL;

            for(int i = 0; i < count; i++) tbw_push_bits_msb(w, (uint64_t)(lens ? lens[i] : 1), 4);

            free(lens);
            break;
        }
    }
}

/* ============================================================
 * ASCII bit reader -- counterpart of TextBitWriter. Needed only now:
 * this is the first time the C side has to INTERPRET a bit-text buffer
 * (the external language dictionary) instead of only producing one.
 * ============================================================ */
typedef struct {
    const char *buf;
    size_t len;
    size_t pos;
} TextBitReader;

static void tbr_init(TextBitReader *r, const char *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

static uint64_t tbr_read_bits(TextBitReader *r, const int nbits) {
    uint64_t v = 0;

    for(int i = 0; i < nbits; i++) {
        v = (v << 1) | (uint64_t)(r->buf[r->pos] == '1' ? 1 : 0);
        r->pos++;
    }

    return v;
}

/* Mirror of ref_enc_write/ref_enc_size_bits: reads one exponential-encoded
 * unsigned int. Same tier loop (widths 4,4,8,16,32,...), each iteration
 * reads exactly `width` new bits and escalates only if that chunk is
 * saturated (equal to max_value) -- see ref_enc_write for the writer side
 * this must stay in exact sync with. */
static int64_t ref_enc_read(TextBitReader *r) {
    int length = 4;

    for(;;) {
        const int ones = (length == 4) ? 0 : length / 2;
        const int width = (length == 4) ? 4 : length / 2;
        const int64_t max_value = (1LL << width) - 1;
        const int64_t val = (int64_t)tbr_read_bits(r, width);

        if(val < max_value) return val + exponential_ones_value(ones);

        length *= 2;
    }
}

/* ============================================================
 * External language alphabet -- parsed from a <lang>.bin buffer already
 * read into memory by Python (this C file never touches the filesystem).
 * Format (fixed by build_language_dict.py, always huffman-len):
 *   A (exp.enc.) + A bytes (8 bit) + (A+1) lengths (4 bit, ids 0..A-1
 *   real chars, id A = escape) + D=0 (exp.enc., always-empty placeholder)
 * ============================================================ */
typedef struct {
    uint8_t alphabet[256];   /* real characters, indexed 0..A-1 */
    int A;                    /* count of real characters (escape excluded) */
    int byte_to_id[256];      /* -1 if this byte is not covered externally */
    HCode *codes;              /* A+1 canonical codes: 0..A-1 real, A = escape */
    int escape_id;             /* == A, kept named for readability at call sites */
} LangAlphabet;

static void lang_alphabet_free(LangAlphabet *la) {
    free(la->codes);
    la->codes = NULL;
}

static LangAlphabet parse_lang_dict(const char *bits, const int32_t bits_len) {
    TextBitReader r;
    tbr_init(&r, bits, (size_t)bits_len);

    LangAlphabet la = {0};

    const int A = (int)ref_enc_read(&r);
    la.A = A;
    la.escape_id = A;

    for(int i = 0; i < 256; i++) la.byte_to_id[i] = -1;

    for(int i = 0; i < A; i++) {
        const uint8_t b = (uint8_t)tbr_read_bits(&r, 8);
        la.alphabet[i] = b;
        la.byte_to_id[b] = i;
    }

    int *lengths = (int *)malloc(sizeof(int) * (A + 1));
    for(int i = 0; i < A + 1; i++) lengths[i] = (int)tbr_read_bits(&r, 4);

    la.codes = canonical_codes(lengths, A + 1);
    free(lengths);

    /* D=0 placeholder that always follows a language dictionary (it never
     * has fragments) -- read and discard, just to stay in sync with the
     * writer's format and to leave r at a well-defined end position. */
    (void)ref_enc_read(&r);

    return la;
}

/* ============================================================
 * Supplemental (local) alphabet -- built on the fly from whichever bytes
 * of THIS program are not covered by the external language alphabet.
 * Real frequencies from the actual program are used here (unlike the
 * language dictionary's Witten-Bell escape estimate): we have the real
 * program in hand, no need to guess.
 * ============================================================ */
typedef struct {
    uint8_t alphabet[256];
    int A;
    int byte_to_id[256];
    HCode *codes;   /* A canonical codes, no escape needed here */
} SupplAlphabet;

static void suppl_alphabet_free(SupplAlphabet *sa) {
    free(sa->codes);
    sa->codes = NULL;
}

static SupplAlphabet build_supplemental_alphabet(const StrItem *strs, const int n, const LangAlphabet *lang) {
    int64_t freq[256] = {0};
    int present[256] = {0};

    for(int s = 0; s < n; s++) {
        for(size_t k = 0; k < strs[s].len; k++) {
            const uint8_t b = strs[s].data[k];

            if(lang->byte_to_id[b] < 0) {
                freq[b]++;
                present[b] = 1;
            }
        }
    }

    SupplAlphabet sa = {0};
    for(int i = 0; i < 256; i++) sa.byte_to_id[i] = -1;

    int A = 0;
    for(int b = 0; b < 256; b++) {
        if(present[b]) {
            sa.alphabet[A] = (uint8_t)b;
            sa.byte_to_id[b] = A;
            A++;
        }
    }
    sa.A = A;

    if(A > 0) {
        int64_t *freq_by_id = (int64_t *)malloc(sizeof(int64_t) * A);
        for(int i = 0; i < A; i++) freq_by_id[i] = freq[sa.alphabet[i]];

        int *lengths = huffman_len_lengths(freq_by_id, A);
        sa.codes = canonical_codes(lengths, A);

        free(freq_by_id);
        free(lengths);
    }

    return sa;
}

/* Combines external + escape + supplemental into a single per-byte cost
 * table, exactly what the untouched search/scoring machinery above
 * expects as char_bit_len_by_byte -- it has no idea (and doesn't need to
 * know) that this cost now comes from two different sources. */
static void compute_hybrid_char_bit_lengths(const LangAlphabet *lang, const SupplAlphabet *suppl, const int escape_length, int *byte_len_out /* [256] */) {
    for(int b = 0; b < 256; b++) {
        if(lang->byte_to_id[b] >= 0) {
            byte_len_out[b] = lang->codes[lang->byte_to_id[b]].length;
        } else if(suppl->byte_to_id[b] >= 0) {
            byte_len_out[b] = escape_length + suppl->codes[suppl->byte_to_id[b]].length;
        } else {
            byte_len_out[b] = 0; /* never referenced: byte unused by this program */
        }
    }
}

/* Writes one RAW character: external code if covered, else escape code
 * followed immediately by the supplemental code -- no extra flag bit
 * needed, the escape id itself (decoded from the external Huffman tree)
 * IS the flag. Used both for literal bytes inside dictionary fragment
 * entries and for RAW symbols in the per-string stream. */
static void write_hybrid_char_symbol(TextBitWriter *w, const LangAlphabet *lang, const SupplAlphabet *suppl, const uint8_t byte_val) {
    const int lang_id = lang->byte_to_id[byte_val];

    if(lang_id >= 0) {
        tbw_push_bits_msb(w, lang->codes[lang_id].code, lang->codes[lang_id].length);
        return;
    }

    const HCode *esc = &lang->codes[lang->escape_id];
    tbw_push_bits_msb(w, esc->code, esc->length);

    const int suppl_id = suppl->byte_to_id[byte_val];
    tbw_push_bits_msb(w, suppl->codes[suppl_id].code, suppl->codes[suppl_id].length);
}

/* Supplemental alphabet section: same block shape used everywhere else in
 * the project (count + raw bytes + 4-bit lengths), just applied to the
 * (usually empty) set of bytes the external language alphabet doesn't
 * cover. A=0 -> just "reference_encoding(0)", 4 fixed bits, matching the
 * same convention already used for D=0 elsewhere. */
static void write_supplemental_alphabet_ascii(TextBitWriter *w, const SupplAlphabet *suppl) {
    ref_enc_write(w, suppl->A);

    for(int i = 0; i < suppl->A; i++) tbw_push_bits_msb(w, suppl->alphabet[i], 8);
    for(int i = 0; i < suppl->A; i++) tbw_push_bits_msb(w, (uint64_t)suppl->codes[i].length, 4);
}

/* Fragments-only section (no alphabet here -- that lives in the external
 * file). Structurally identical to the old write_dict_section_ascii minus
 * the alphabet part, with literal fragment bytes going through
 * write_hybrid_char_symbol instead of a single flat Huffman table. */
static void write_fragments_section_hybrid_ascii(TextBitWriter *w, const LangAlphabet *lang, const SupplAlphabet *suppl, const Dictionary *dict, const int64_t *tok_freqs) {
    const int D = dict->n;
    ref_enc_write(w, D);

    for(int i = 0; i < D; i++) {
        ref_enc_write(w, dict->entries[i].len);

        for(int k = 0; k < dict->entries[i].len; k++) {
            write_hybrid_char_symbol(w, lang, suppl, dict->entries[i].data[k]);
        }
    }

    codec_write_overhead_ascii(ENC_HUFF_LEN, w, tok_freqs, D);
}

/* Per-string stream: RAW goes through write_hybrid_char_symbol, TOK
 * reuses the existing generic Huffman symbol writer unchanged (fragment
 * references were never affected by any of this -- always their own,
 * single, program-specific Huffman table). */
static char *write_seq_hybrid_ascii(const Seq *seq, const LangAlphabet *lang, const SupplAlphabet *suppl, const HCode *tok_codes, int32_t *out_len) {
    TextBitWriter w;
    tbw_init(&w, (size_t)seq->len * 4 + 16);

    ref_enc_write(&w, seq->len);

    for(int k = 0; k < seq->len; k++) {
        if(seq->items[k].type == RAW) {
            tbw_push_bits_msb(&w, 0, 1);
            write_hybrid_char_symbol(&w, lang, suppl, (uint8_t)seq->items[k].val);
        } else {
            tbw_push_bits_msb(&w, 1, 1);
            codec_write_symbol_ascii(ENC_HUFF_LEN, &w, seq->items[k].val, tok_codes, 0);
        }
    }

    *out_len = (int32_t)w.len;
    return tbw_finish(&w);
}

/* ============================================================
 * Library API -- hybrid branch entry point.
 *
 * Unlike qrtree_build's local/external siblings, this branch never
 * constructs its own alphabet from the program: it always receives one,
 * already parsed from a <lang>.bin buffer Python has read from disk (this
 * file still never touches the filesystem). Everything downstream of
 * "here is a per-byte bit cost table" -- the whole greedy/DFS fragment
 * search, unchanged above -- has no idea the cost table now blends an
 * external Huffman tree with a small local escape-triggered one.
 * ============================================================ */
typedef struct {
    char *dict_bits;         /* lang_id + supplemental alphabet + fragments + token overhead */
    int32_t dict_bits_len;    /* (does NOT include the "10" mode selector -- Python writes that) */

    char **stream_bits;       /* n_strings ASCII '0'/'1' strings, SAME ORDER as input strings */
    int32_t *stream_bits_len;
    int32_t n_strings;
} QRTreeHybridResult;

/*
 * strings/string_lens: n_strings byte buffers with the program's literal
 * strings, in source order.
 * lang_dict_bits/lang_dict_bits_len: the ENTIRE content of <lang>.bin,
 *   already read into memory by Python.
 * lang_id: the 3-bit language id to embed in the header (0 = default),
 *   purely an application-level convention -- this file does not
 *   interpret it, only writes it.
 * exh_max_depth: 0=greedy, -1=unlimited DFS, N=depth N. nthreads<=0 keeps
 * the current setting.
 */
QRTreeHybridResult *qrtree_compress_program_hybrid(const uint8_t **strings, const int32_t *string_lens, const int32_t n_strings,
                                                    const char *lang_dict_bits, const int32_t lang_dict_bits_len, const int32_t lang_id,
                                                    const int32_t min_len, const int32_t max_len, const int32_t max_dict,
                                                    const int32_t exh_max_depth, const int32_t nthreads) {
    if(nthreads > 0) g_nthreads = nthreads;

    ThreadPool pool;
    pool_init(&pool, g_nthreads);
    g_pool = &pool;

    StrItem *strs = (StrItem *)malloc(sizeof(StrItem) * (n_strings > 0 ? n_strings : 1));
    for(int i = 0; i < n_strings; i++) {
        strs[i].data = (uint8_t *)strings[i];
        strs[i].len = (size_t)string_lens[i];
    }

    LangAlphabet lang = parse_lang_dict(lang_dict_bits, lang_dict_bits_len);
    SupplAlphabet suppl = build_supplemental_alphabet(strs, n_strings, &lang);
    const int escape_length = lang.codes[lang.escape_id].length;

    int char_bit_len_by_byte[256] = {0};
    compute_hybrid_char_bit_lengths(&lang, &suppl, escape_length, char_bit_len_by_byte);

    const int max_depth_is_none = (exh_max_depth < 0);
    const int max_depth = max_depth_is_none ? 0 : exh_max_depth;

    Dictionary dictionary; SeqList seqs;
    exhaustive_build(strs, n_strings, char_bit_len_by_byte, ENC_HUFF_LEN, min_len, max_len, max_dict,
                      max_depth, max_depth_is_none, &dictionary, &seqs);

    int64_t *tok_freqs = count_tok_freqs(&seqs, dictionary.n);
    int *tok_lengths_by_id = dictionary.n > 0 ? huffman_len_lengths(tok_freqs, dictionary.n) : NULL;
    HCode *tok_codes = dictionary.n > 0 ? canonical_codes(tok_lengths_by_id, dictionary.n) : NULL;

    TextBitWriter dict_w;
    tbw_init(&dict_w, 4096);

    ref_enc_write(&dict_w, lang_id);
    write_supplemental_alphabet_ascii(&dict_w, &suppl);
    write_fragments_section_hybrid_ascii(&dict_w, &lang, &suppl, &dictionary, tok_freqs);

    QRTreeHybridResult *out = (QRTreeHybridResult *)malloc(sizeof(QRTreeHybridResult));
    out->dict_bits = tbw_finish(&dict_w);
    out->dict_bits_len = (int32_t)dict_w.len;

    out->n_strings = seqs.n;
    out->stream_bits = (char **)malloc(sizeof(char *) * (seqs.n > 0 ? seqs.n : 1));
    out->stream_bits_len = (int32_t *)malloc(sizeof(int32_t) * (seqs.n > 0 ? seqs.n : 1));

    for(int i = 0; i < seqs.n; i++) {
        out->stream_bits[i] = write_seq_hybrid_ascii(&seqs.seqs[i], &lang, &suppl, tok_codes, &out->stream_bits_len[i]);
    }

    free(tok_lengths_by_id);
    free(tok_codes);
    free(tok_freqs);

    lang_alphabet_free(&lang);
    suppl_alphabet_free(&suppl);

    dict_free(&dictionary);
    seqlist_free(&seqs);
    free(strs);

    g_pool = NULL;
    pool_shutdown(&pool);

    return out;
}

void qrtree_compress_program_hybrid_free(QRTreeHybridResult *r) {
    if(!r) return;

    free(r->dict_bits);

    for(int i = 0; i < r->n_strings; i++) free(r->stream_bits[i]);
    free(r->stream_bits);
    free(r->stream_bits_len);

    free(r);
}