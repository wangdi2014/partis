
/* Maybe:
 * - read a batch of sequences
 * - Create some threads
 * - process / write
 * - read more
 */

/*  main.c
 *  Created by Mengyao Zhao on 06/23/11.
 *	Version 0.1.4
 *  Last revision by Mengyao Zhao on 07/31/12.
 */

#include <stdlib.h>
#include <stdint.h>
#include <emmintrin.h>
#include <zlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include "kseq.h"
#include "ksw.h"
#include "kvec.h"

#ifdef __GNUC__
#define LIKELY(x) __builtin_expect((x),1)
#define UNLIKELY(x) __builtin_expect((x),0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/*! @function
  @abstract  Round an integer to the next closest power-2 integer.
  @param  x  integer to be rounded (in place)
  @discussion x will be modified.
  */
#define kroundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))

KSEQ_INIT(gzFile, gzread);

typedef kvec_t(kseq_t*) kseq_v;

/* Destroy a vector of pointers, calling f on each item */
#define kvp_destroy(f, v) \
    for(size_t __kpd = 0; __kpd < kv_size(v); __kpd++) \
        f(kv_A(v, __kpd)); \
    kv_destroy(v)

kseq_v read_all_seqs(gzFile fp) {
    kseq_t *seq = kseq_init(fp);
    kseq_v result;
    kv_init(result);
    int l;
    while((l = kseq_read(seq)) > 0) {
        kv_push(kseq_t*, result, seq);
        seq = kseq_init(fp);
    }
    kseq_destroy(seq); // one extra
    return result;
}


kseq_v read_seqs(gzFile fp,
                 size_t n_wanted) {
    kseq_t *seq = kseq_init(fp);
    kseq_v result;
    kv_init(result);
    for(size_t i = 0; i < n_wanted; i++) {
        const int l = kseq_read(seq);
        if(l <= 0) {
            break;
        }
        kv_push(kseq_t*, result, seq);
        seq = kseq_init(fp);
    }
    kseq_destroy(seq); // one extra
    return result;
}

typedef struct {
    size_t target_idx;
    kswr_t loc;
    uint32_t *cigar;
    int n_cigar;
} aln_t;
typedef kvec_t(aln_t) aln_v;

typedef struct {
    aln_v alignments;
} aln_task_t;
typedef kvec_t(aln_task_t) aln_task_v;

typedef struct {
    int tid;
    int n_threads;
    int32_t gap_o; /* 3 */
    int32_t gap_e; /* 1 */
    uint8_t *table;
    int m; /* Number of residue tyes */
    int8_t *mat; /* Scoring matrix */
    kseq_v targets;
    kseq_v reads;
    aln_v* alignments;
} thread_aux_t;

typedef struct {
    int32_t gap_o; /* 3 */
    int32_t gap_e; /* 1 */
    uint8_t *table;
    int m; /* Number of residue tyes */
    int8_t *mat; /* Scoring matrix */
} align_config_t;

int ascore_lt_dec(const void* va, const void* vb) {
    const aln_t* a = (const aln_t*) va;
    const aln_t* b = (const aln_t*) vb;
    if(a->loc.score == b->loc.score) return 0;
    if(a->loc.score < b->loc.score) return 1;
    return -1;
}

static aln_v align_read(const kseq_t* read,
                        const kseq_v targets,
                        const align_config_t *conf)
{
    kseq_t *r;
    const int32_t read_len = read->seq.l;

    aln_v result;
    kv_init(result);
    kv_resize(aln_t, result, kv_size(targets));

    uint8_t *read_num = calloc(read_len, sizeof(uint8_t));

    for (size_t k = 0; k < read_len; ++k)
        read_num[k] = conf->table[(int)read->seq.s[k]];

    // Align to each target
    kswq_t *qry = NULL;
    for(size_t j = 0; j < kv_size(targets); j++) {
        // Encode target
        r = kv_A(targets, j);
        uint8_t *ref_num = calloc(r->seq.l, sizeof(uint8_t));
        for (size_t k = 0; k < r->seq.l; ++k)
            ref_num[k] = conf->table[(int)r->seq.s[k]];

        aln_t aln;
        aln.target_idx = j;
        aln.loc = ksw_align(read_len, read_num,
                            r->seq.l, ref_num,
                            conf->m,
                            conf->mat,
                            conf->gap_o,
                            conf->gap_e,
                            KSW_XSTART,
                            &qry);
        ksw_global(aln.loc.qe - aln.loc.qb,
                   read_num + aln.loc.qb,
                   aln.loc.te - aln.loc.tb,
                   ref_num + aln.loc.tb,
                   conf->m,
                   conf->mat,
                   conf->gap_o,
                   conf->gap_e,
                   20,
                   &aln.n_cigar,
                   &aln.cigar);
        kv_push(aln_t, result, aln);
    }
    free(qry);
    qsort(result.a,
          kv_size(result),
          sizeof(aln_t),
          ascore_lt_dec);
    return result;
}

void align_reads (const char* ref_path,
                  const char* qry_path,
                  const char* output_path,
                  const int32_t match,    /* 2 */
                  const int32_t mismatch, /* 2 */
                  const int32_t gap_o,    /* 3 */
                  const int32_t gap_e,    /* 1 */
                  const uint8_t n_threads) { /* 1 */
    gzFile read_fp, ref_fp, out_fp;
    int32_t j, l, k;
    const int m = 5;
    int8_t* mat = (int8_t*)calloc(25, sizeof(int8_t));

    /* This table is used to transform nucleotide letters into numbers. */
    uint8_t table[128] = {
        4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
        4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
        4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
        4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
        4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
        4, 4, 4, 4,  3, 0, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
        4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
        4, 4, 4, 4,  3, 0, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
    };

    // initialize scoring matrix for genome sequences
    for (l = k = 0; LIKELY(l < 4); ++l) {
        for (j = 0; LIKELY(j < 4); ++j) mat[k++] = l == j ? match : -mismatch;	/* weight_match : -weight_mismatch */
        mat[k++] = 0; // ambiguous base
    }
    for (j = 0; LIKELY(j < 5); ++j) mat[k++] = 0;


    // Read reference sequences

    ref_fp = gzopen(ref_path, "r");
    kseq_v ref_seqs;
    ref_seqs = read_all_seqs(ref_fp);
    gzclose(ref_fp);

    // Print SAM header
    out_fp = gzopen(output_path, "w0");

    align_config_t conf;
    conf.gap_o = gap_o;
    conf.gap_e = gap_e;
    conf.m = m;
    conf.table = table;
    conf.mat = mat;

    read_fp = gzopen(qry_path, "r");
    while(true) {
        kseq_v reads = read_seqs(read_fp, 10000); // TODO: Magic number
        if(!kv_size(reads))
            break;
        const size_t n_reads = kv_size(reads);
        for(size_t i = 0; i < n_reads; i++) {
            printf("Aligning %8lu\r", i);
            aln_v result = align_read(kv_A(reads, i),
                                      ref_seqs,
                                      &conf);
            for(size_t j = 0; j < kv_size(result); j++)
                free(kv_A(result, j).cigar);
            kv_destroy(result);
        }
        kvp_destroy(kseq_destroy, reads);
    }

    // Clean up reference sequences
    kvp_destroy(kseq_destroy, ref_seqs);

    gzclose(read_fp);
    gzclose(out_fp);
    free(mat);
}
