/*
 * Copyright (c) 2014-2019 Genome Research Ltd.
 * Author(s): James Bonfield
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *    3. Neither the names Genome Research Ltd and Wellcome Trust Sanger
 *       Institute nor the names of its contributors may be used to endorse
 *       or promote products derived from this software without specific
 *       prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY GENOME RESEARCH LTD AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL GENOME RESEARCH
 * LTD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if defined(NO_THREADS) && (defined(__APPLE__) || defined(_WIN32))
// When pthreads is available, we use a single malloc, otherwise we'll
// (normally) use the stack instead.
//
// However the MacOS X default stack size can be tiny (512K), albeit
// I think only when threading?  We request malloc/free for the large
// local arrays instead to avoid this, but it does have a performance hit.
#define USE_HEAP
#endif

// Use 11 for order-1?
#define TF_SHIFT 12
#define TOTFREQ (1<<TF_SHIFT)

#include "rANS_byte.h"

/*-------------------------------------------------------------------------- */
/*
 * Example wrapper to use the rans_byte.h functions included above.
 *
 * This demonstrates how to use, and unroll, an order-0 and order-1 frequency
 * model.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <sys/time.h>
#ifndef NO_THREADS
#include <pthread.h>
#endif

#include "rANS_static.h"

#define ABS(a) ((a)>0?(a):-(a))

/*-----------------------------------------------------------------------------
 * Memory to memory compression functions.
 *
 * These are original versions without any manual loop unrolling. They
 * are easier to understand, but can be up to 2x slower.
 */

#define MAGIC 8

static void hist8(unsigned char *in, unsigned int in_size, int F0[256]) {
    int F1[256+MAGIC] = {0}, F2[256+MAGIC] = {0}, F3[256+MAGIC] = {0};
    int F4[256+MAGIC] = {0}, F5[256+MAGIC] = {0}, F6[256+MAGIC] = {0}, F7[256+MAGIC] = {0};
    int i, i8 = in_size & ~7;
    for (i = 0; i < i8; i+=8) {
	F0[in[i+0]]++;
	F1[in[i+1]]++;
	F2[in[i+2]]++;
	F3[in[i+3]]++;
	F4[in[i+4]]++;
	F5[in[i+5]]++;
	F6[in[i+6]]++;
	F7[in[i+7]]++;
    }
    while (i < in_size)
	F0[in[i++]]++;

    for (i = 0; i < 256; i++)
	F0[i] += F1[i] + F2[i] + F3[i] + F4[i] + F5[i] + F6[i] + F7[i];
}

static
unsigned char *rans_compress_O0(unsigned char *in, unsigned int in_size,
				unsigned int *out_size) {
    unsigned char *out_buf = malloc(1.05*in_size + 257*257*3 + 9);
    unsigned char *cp, *out_end;
    RansEncSymbol syms[256];
    RansState rans0;
    RansState rans2;
    RansState rans1;
    RansState rans3;
    uint8_t* ptr;
    int F[256+MAGIC] = {0}, i, j, tab_size, rle, x, fsum = 0;
    int m = 0, M = 0;
    uint64_t tr;

    if (!out_buf)
	return NULL;

    ptr = out_end = out_buf + (int)(1.05*in_size) + 257*257*3 + 9;

    // Compute statistics
    hist8(in, in_size, F);
    tr = ((uint64_t)TOTFREQ<<31)/in_size + (1<<30)/in_size;

 normalise_harder:
    // Normalise so T[i] == TOTFREQ
    for (fsum = m = M = j = 0; j < 256; j++) {
	if (!F[j])
	    continue;

	if (m < F[j])
	    m = F[j], M = j;

	if ((F[j] = (F[j]*tr)>>31) == 0)
	    F[j] = 1;
	fsum += F[j];
    }

    fsum++;
    if (fsum < TOTFREQ) {
	F[M] += TOTFREQ-fsum;
    } else if (fsum-TOTFREQ > F[M]/2) {
	// Corner case to avoid excessive frequency reduction
	tr = 2104533975; goto normalise_harder; // equiv to *0.98.
    } else {
	F[M] -= fsum-TOTFREQ;
    }

    //printf("F[%d]=%d\n", M, F[M]);
    assert(F[M]>0);

    // Encode statistics.
    cp = out_buf+9;

    for (x = rle = j = 0; j < 256; j++) {
	if (F[j]) {
	    // j
	    if (rle) {
		rle--;
	    } else {
		*cp++ = j;
		if (!rle && j && F[j-1])  {
		    for(rle=j+1; rle<256 && F[rle]; rle++)
			;
		    rle -= j+1;
		    *cp++ = rle;
		}
		//fprintf(stderr, "%d: %d %d\n", j, rle, N[j]);
	    }
	    
	    // F[j]
	    if (F[j]<128) {
		*cp++ = F[j];
	    } else {
		*cp++ = 128 | (F[j]>>8);
		*cp++ = F[j]&0xff;
	    }
	    RansEncSymbolInit(&syms[j], x, F[j], TF_SHIFT);
	    x += F[j];
	}
    }
    *cp++ = 0;

    //write(2, out_buf+4, cp-(out_buf+4));
    tab_size = cp-out_buf;

    RansEncInit(&rans0);
    RansEncInit(&rans1);
    RansEncInit(&rans2);
    RansEncInit(&rans3);

    switch (i=(in_size&3)) {
    case 3: RansEncPutSymbol(&rans2, &ptr, &syms[in[in_size-(i-2)]]);
    case 2: RansEncPutSymbol(&rans1, &ptr, &syms[in[in_size-(i-1)]]);
    case 1: RansEncPutSymbol(&rans0, &ptr, &syms[in[in_size-(i-0)]]);
    case 0:
	break;
    }
    for (i=(in_size &~3); i>0; i-=4) {
	RansEncSymbol *s3 = &syms[in[i-1]];
	RansEncSymbol *s2 = &syms[in[i-2]];
	RansEncSymbol *s1 = &syms[in[i-3]];
	RansEncSymbol *s0 = &syms[in[i-4]];

	RansEncPutSymbol(&rans3, &ptr, s3);
	RansEncPutSymbol(&rans2, &ptr, s2);
	RansEncPutSymbol(&rans1, &ptr, s1);
	RansEncPutSymbol(&rans0, &ptr, s0);
    }

    RansEncFlush(&rans3, &ptr);
    RansEncFlush(&rans2, &ptr);
    RansEncFlush(&rans1, &ptr);
    RansEncFlush(&rans0, &ptr);

    // Finalise block size and return it
    *out_size = (out_end - ptr) + tab_size;

    cp = out_buf;

    *cp++ = 0; // order
    *cp++ = ((*out_size-9)>> 0) & 0xff;
    *cp++ = ((*out_size-9)>> 8) & 0xff;
    *cp++ = ((*out_size-9)>>16) & 0xff;
    *cp++ = ((*out_size-9)>>24) & 0xff;

    *cp++ = (in_size>> 0) & 0xff;
    *cp++ = (in_size>> 8) & 0xff;
    *cp++ = (in_size>>16) & 0xff;
    *cp++ = (in_size>>24) & 0xff;

    memmove(out_buf + tab_size, ptr, out_end-ptr);

    return out_buf;
}

typedef struct {
    unsigned char R[TOTFREQ];
} ari_decoder;

static
unsigned char *rans_uncompress_O0(unsigned char *in, unsigned int in_size,
				  unsigned int *out_size) {
    /* Load in the static tables */
    unsigned char *cp = in + 9;
    unsigned char *cp_end = in + in_size;
    const uint32_t mask = (1u << TF_SHIFT)-1;
    int i, j, rle;
    unsigned int x, y;
    unsigned int out_sz, in_sz;
    char *out_buf;
    RansState R[4];
    RansState m[4];
    uint16_t sfreq[TOTFREQ+32];
    uint16_t ssym [TOTFREQ+32]; // faster, but only needs uint8_t
    uint32_t sbase[TOTFREQ+16]; // faster, but only needs uint16_t

    if (in_size < 26) // Need at least this many bytes just to start
        return NULL;

    if (*in++ != 0) // Order-0 check
	return NULL;
    
    in_sz  = ((in[0])<<0) | ((in[1])<<8) | ((in[2])<<16) | ((in[3])<<24);
    out_sz = ((in[4])<<0) | ((in[5])<<8) | ((in[6])<<16) | ((in[7])<<24);
    if (in_sz != in_size-9)
	return NULL;

    if (out_sz >= INT_MAX)
	return NULL; // protect against some overflow cases

    // For speeding up the fuzzer only.
    // Small input can lead to large uncompressed data.
    // We reject this as it just slows things up instead of testing more code
    // paths (once we've verified a few times for large data).
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (out_sz > 100000)
	return NULL;
#endif

    out_buf = malloc(out_sz);
    if (!out_buf)
	return NULL;

    //fprintf(stderr, "out_sz=%d\n", out_sz);

    // Precompute reverse lookup of frequency.
    rle = x = y = 0;
    j = *cp++;
    do {
	int F, C;
        if (cp > cp_end - 16) goto cleanup; // Not enough input bytes left
	if ((F = *cp++) >= 128) {
	    F &= ~128;
	    F = ((F & 127) << 8) | *cp++;
	}
	C = x;

	if (x + F > TOTFREQ)
	    goto cleanup;

        for (y = 0; y < F; y++) {
            ssym [y + C] = j;
            sfreq[y + C] = F;
            sbase[y + C] = y;
        }
	x += F;

	if (!rle && j+1 == *cp) {
	    j = *cp++;
	    rle = *cp++;
	} else if (rle) {
	    rle--;
	    j++;
	    if (j > 255)
		goto cleanup;
	} else {
	    j = *cp++;
	}
    } while(j);

    if (x < TOTFREQ-1 || x > TOTFREQ)
	goto cleanup;

    // 16 bytes of cp here. Also why cp - 16 in above loop.
    if (cp > cp_end - 16) goto cleanup; // Not enough input bytes left

    RansDecInit(&R[0], &cp); if (R[0] < RANS_BYTE_L) goto cleanup;
    RansDecInit(&R[1], &cp); if (R[1] < RANS_BYTE_L) goto cleanup;
    RansDecInit(&R[2], &cp); if (R[2] < RANS_BYTE_L) goto cleanup;
    RansDecInit(&R[3], &cp); if (R[3] < RANS_BYTE_L) goto cleanup;

    int out_end = (out_sz&~3);
    cp_end -= 8; // within 8 for simplicity of loop below
    for (i=0; i < out_end; i+=4) {
	m[0] = R[0] & mask;
        out_buf[i+0] = ssym[m[0]];
        R[0] = sfreq[m[0]] * (R[0] >> TF_SHIFT) + sbase[m[0]];

        m[1] = R[1] & mask;
	out_buf[i+1] = ssym[m[1]];
        R[1] = sfreq[m[1]] * (R[1] >> TF_SHIFT) + sbase[m[1]];

        m[2] = R[2] & mask;
	out_buf[i+2] = ssym[m[2]];
        R[2] = sfreq[m[2]] * (R[2] >> TF_SHIFT) + sbase[m[2]];

        m[3] = R[3] & mask;
	out_buf[i+3] = ssym[m[3]];
        R[3] = sfreq[m[3]] * (R[3] >> TF_SHIFT) + sbase[m[3]];

	if (cp < cp_end) {
	    RansDecRenorm2(&R[0], &R[1], &cp);
	    RansDecRenorm2(&R[2], &R[3], &cp);
	} else {
	    RansDecRenormSafe(&R[0], &cp, cp_end+8);
	    RansDecRenormSafe(&R[1], &cp, cp_end+8);
	    RansDecRenormSafe(&R[2], &cp, cp_end+8);
	    RansDecRenormSafe(&R[3], &cp, cp_end+8);
	}
    }

    switch(out_sz&3) {
    case 3:
        out_buf[out_end + 2] = ssym[R[2] & mask];
    case 2:
        out_buf[out_end + 1] = ssym[R[1] & mask];
    case 1:
        out_buf[out_end] = ssym[R[0] & mask];
    default:
        break;
    }
    
    *out_size = out_sz;
    return (unsigned char *)out_buf;

 cleanup:
    free(out_buf);
    return NULL;
}

static void hist1_4(unsigned char *in, unsigned int in_size,
		    int F0[256][256], int *T0) {
    int T1[256+MAGIC] = {0}, T2[256+MAGIC] = {0}, T3[256+MAGIC] = {0};
    unsigned int idiv4 = in_size/4;
    int i;
    unsigned char c0, c1, c2, c3;

    unsigned char *in0 = in + 0;
    unsigned char *in1 = in + idiv4;
    unsigned char *in2 = in + idiv4*2;
    unsigned char *in3 = in + idiv4*3;

    unsigned char last_0 = 0, last_1 = in1[-1], last_2 = in2[-1], last_3 = in3[-1];
    //unsigned char last_0 = 0, last_1 = 0, last_2 = 0, last_3 = 0;

    unsigned char *in0_end = in1;

    while (in0 < in0_end) {
	F0[last_0][c0 = *in0++]++;
	T0[last_0]++;
	last_0 = c0;

	F0[last_1][c1 = *in1++]++;
	T1[last_1]++;
	last_1 = c1;

	F0[last_2][c2 = *in2++]++;
	T2[last_2]++;
	last_2 = c2;

	F0[last_3][c3 = *in3++]++;
	T3[last_3]++;
	last_3 = c3;
    }

    while (in3 < in + in_size) {
	F0[last_3][c3 = *in3++]++;
	T3[last_3]++;
	last_3 = c3;
    }

    for (i = 0; i < 256; i++) {
	T0[i]+=T1[i]+T2[i]+T3[i];
    }
}

#ifndef NO_THREADS
/*
 * Thread local storage per thread in the pool.
 * This avoids needing to memset/calloc F and syms in the encoder,
 * which can be speed things this encoder up a little.
 */
static pthread_once_t rans_enc_once = PTHREAD_ONCE_INIT;
static pthread_key_t rans_enc_key;

typedef struct {
    RansEncSymbol (*syms)[256];
    int (*F)[256];
} thread_enc_data;

static void rans_enc_free(void *vp) {
    thread_enc_data *te = (thread_enc_data *)vp;
    if (!te)
	return;
    free(te->F);
    free(te->syms);
    free(te);
}

static thread_enc_data *rans_enc_alloc(void) {
    thread_enc_data *te = malloc(sizeof(*te));
    if (!te)
	return NULL;
    te->F = calloc(256, sizeof(*te->F));
    te->syms = calloc(256, sizeof(*te->syms));
    if (!te->F || !te->syms) {
	rans_enc_free(te);
	return NULL;
    }

    return te;
}

static void rans_tls_enc_init(void) {
    pthread_key_create(&rans_enc_key, rans_enc_free);
}
#endif

static
unsigned char *rans_compress_O1(unsigned char *in, unsigned int in_size,
				unsigned int *out_size) {
    unsigned char *out_buf = NULL, *out_end, *cp;
    unsigned int tab_size, rle_i, rle_j;


#ifndef NO_THREADS
    pthread_once(&rans_enc_once, rans_tls_enc_init);
    thread_enc_data *te = pthread_getspecific(rans_enc_key);
    if (!te) {
	if (!(te = rans_enc_alloc()))
	    return NULL;
	pthread_setspecific(rans_enc_key, te);
    }
    RansEncSymbol (*syms)[256] = te->syms;
    int (*F)[256] = te->F;
    memset(F, 0, 256*sizeof(*F));
#else
#ifdef USE_HEAP
    RansEncSymbol (*syms)[256] = malloc(256 * sizeof(*syms));
    int (*F)[256] = calloc(256, sizeof(*F));
#else
    RansEncSymbol syms[256][256];
    int F[256][256] = {{0}};
#endif
#endif
    int T[256+MAGIC] = {0};
    int i, j;

    if (in_size < 4)
	return rans_compress_O0(in, in_size, out_size);

#ifdef USE_HEAP
    if (!syms) goto cleanup;
    if (!F) goto cleanup;
#endif

    out_buf = malloc(1.05*in_size + 257*257*3 + 9);
    if (!out_buf) goto cleanup;

    out_end = out_buf + (int)(1.05*in_size) + 257*257*3 + 9;
    cp = out_buf+9;

    hist1_4(in, in_size, F, T);

    F[0][in[1*(in_size>>2)]]++;
    F[0][in[2*(in_size>>2)]]++;
    F[0][in[3*(in_size>>2)]]++;
    T[0]+=3;

    
    // Normalise so T[i] == TOTFREQ
    for (rle_i = i = 0; i < 256; i++) {
	int t2, m, M;
	unsigned int x;

	if (T[i] == 0)
	    continue;

	//uint64_t p = (TOTFREQ * TOTFREQ) / t;
	double p = ((double)TOTFREQ)/T[i];
    normalise_harder:
	for (t2 = m = M = j = 0; j < 256; j++) {
	    if (!F[i][j])
		continue;

	    if (m < F[i][j])
		m = F[i][j], M = j;

	    //if ((F[i][j] = (F[i][j] * p) / TOTFREQ) == 0)
	    if ((F[i][j] *= p) == 0)
		F[i][j] = 1;
	    t2 += F[i][j];
	}

	t2++;
	if (t2 < TOTFREQ) {
	    F[i][M] += TOTFREQ-t2;
	} else if (t2-TOTFREQ >= F[i][M]/2) {
	    // Corner case to avoid excessive frequency reduction
	    p = .98; goto normalise_harder;
	} else {
	    F[i][M] -= t2-TOTFREQ;
	}

	// Store frequency table
	// i
	if (rle_i) {
	    rle_i--;
	} else {
	    *cp++ = i;
	    // FIXME: could use order-0 statistics to observe which alphabet
	    // symbols are present and base RLE on that ordering instead.
	    if (i && T[i-1]) {
		for(rle_i=i+1; rle_i<256 && T[rle_i]; rle_i++)
		    ;
		rle_i -= i+1;
		*cp++ = rle_i;
	    }
	}

	int *F_i_ = F[i];
	x = 0;
	rle_j = 0;
	for (j = 0; j < 256; j++) {
	    if (F_i_[j]) {
		//fprintf(stderr, "F[%d][%d]=%d, x=%d\n", i, j, F_i_[j], x);

		// j
		if (rle_j) {
		    rle_j--;
		} else {
		    *cp++ = j;
		    if (!rle_j && j && F_i_[j-1]) {
			for(rle_j=j+1; rle_j<256 && F_i_[rle_j]; rle_j++)
			    ;
			rle_j -= j+1;
			*cp++ = rle_j;
		    }
		}

		// F_i_[j]
		if (F_i_[j]<128) {
 		    *cp++ = F_i_[j];
		} else {
		    *cp++ = 128 | (F_i_[j]>>8);
		    *cp++ = F_i_[j]&0xff;
		}

		RansEncSymbolInit(&syms[i][j], x, F_i_[j], TF_SHIFT);
		x += F_i_[j];
	    }
	}
	*cp++ = 0;
    }
    *cp++ = 0;

    //write(2, out_buf+4, cp-(out_buf+4));
    tab_size = cp - out_buf;
    assert(tab_size < 257*257*3);
    
    RansState rans0, rans1, rans2, rans3;
    RansEncInit(&rans0);
    RansEncInit(&rans1);
    RansEncInit(&rans2);
    RansEncInit(&rans3);

    uint8_t* ptr = out_end;

    int isz4 = in_size>>2;
    int i0 = 1*isz4-2;
    int i1 = 2*isz4-2;
    int i2 = 3*isz4-2;
    int i3 = 4*isz4-2;

    unsigned char l0 = in[i0+1];
    unsigned char l1 = in[i1+1];
    unsigned char l2 = in[i2+1];
    unsigned char l3 = in[i3+1];

    // Deal with the remainder
    l3 = in[in_size-1];
    for (i3 = in_size-2; i3 > 4*isz4-2; i3--) {
	unsigned char c3 = in[i3];
	RansEncPutSymbol(&rans3, &ptr, &syms[c3][l3]);
	l3 = c3;
    }

    for (; i0 >= 0; i0--, i1--, i2--, i3--) {
	unsigned char c3 = in[i3];
	unsigned char c2 = in[i2];
	unsigned char c1 = in[i1];
	unsigned char c0 = in[i0];

	RansEncSymbol *s3 = &syms[c3][l3];
	RansEncSymbol *s2 = &syms[c2][l2];
	RansEncSymbol *s1 = &syms[c1][l1];
	RansEncSymbol *s0 = &syms[c0][l0];

	RansEncPutSymbol(&rans3, &ptr, s3);
	RansEncPutSymbol(&rans2, &ptr, s2);
	RansEncPutSymbol(&rans1, &ptr, s1);
	RansEncPutSymbol(&rans0, &ptr, s0);

	l3 = c3;
	l2 = c2;
	l1 = c1;
	l0 = c0;
    }

    RansEncPutSymbol(&rans3, &ptr, &syms[0][l3]);
    RansEncPutSymbol(&rans2, &ptr, &syms[0][l2]);
    RansEncPutSymbol(&rans1, &ptr, &syms[0][l1]);
    RansEncPutSymbol(&rans0, &ptr, &syms[0][l0]);

    RansEncFlush(&rans3, &ptr);
    RansEncFlush(&rans2, &ptr);
    RansEncFlush(&rans1, &ptr);
    RansEncFlush(&rans0, &ptr);

    *out_size = (out_end - ptr) + tab_size;

    cp = out_buf;
    *cp++ = 1; // order

    *cp++ = ((*out_size-9)>> 0) & 0xff;
    *cp++ = ((*out_size-9)>> 8) & 0xff;
    *cp++ = ((*out_size-9)>>16) & 0xff;
    *cp++ = ((*out_size-9)>>24) & 0xff;

    *cp++ = (in_size>> 0) & 0xff;
    *cp++ = (in_size>> 8) & 0xff;
    *cp++ = (in_size>>16) & 0xff;
    *cp++ = (in_size>>24) & 0xff;

    memmove(out_buf + tab_size, ptr, out_end-ptr);

 cleanup:
#ifdef USE_HEAP
    free(syms);
    free(F);
#endif

    return out_buf;
}

#ifndef NO_THREADS
/*
 * Thread local storage per thread in the pool.
 * This avoids needing to memset/calloc D and syms in the decoder,
 * which can be speed things this decoder up a little (~10%).
 */
static pthread_once_t rans_once = PTHREAD_ONCE_INIT;
static pthread_key_t rans_key;

typedef struct {
    ari_decoder *D;
    RansDecSymbol32 (*syms)[256];
} thread_data;

static void rans_tb_free(void *vp) {
    thread_data *tb = (thread_data *)vp;
    if (!tb)
	return;
    free(tb->D);
    free(tb->syms);
    free(tb);
}

static thread_data *rans_tb_alloc(void) {
    thread_data *tb = malloc(sizeof(*tb));
    if (!tb)
	return NULL;
    tb->D = calloc(256, sizeof(*tb->D));
    tb->syms = calloc(256, sizeof(*tb->syms));
    if (!tb->D || !tb->syms) {
	rans_tb_free(tb);
	return NULL;
    }

    return tb;
}

static void rans_tls_init(void) {
    pthread_key_create(&rans_key, rans_tb_free);
}
#endif

static
unsigned char *rans_uncompress_O1(unsigned char *in, unsigned int in_size,
				  unsigned int *out_size) {
    /* Load in the static tables */
    unsigned char *cp = in + 9;
    unsigned char *ptr_end = in + in_size;
    int i, j = -999, rle_i, rle_j;
    unsigned int x;
    unsigned int out_sz, in_sz;
    char *out_buf = NULL;
    // D[] is 1Mb and syms[][] is 0.5Mb.
#ifndef NO_THREADS
    pthread_once(&rans_once, rans_tls_init);
    thread_data *tb = pthread_getspecific(rans_key);
    if (!tb) {
	if (!(tb = rans_tb_alloc()))
	    return NULL;
	pthread_setspecific(rans_key, tb);
    }
    ari_decoder *const D = tb->D;
    RansDecSymbol32 (*const syms)[256] = tb->syms;
#else
#ifdef USE_HEAP
    //ari_decoder *const D = malloc(256 * sizeof(*D));
    ari_decoder *const D = calloc(256, sizeof(*D));
    RansDecSymbol32 (*const syms)[256] = malloc(256 * sizeof(*syms));
    for (i = 1; i < 256; i++) memset(&syms[i][0], 0, sizeof(syms[0][0]));
#else
    ari_decoder D[256] = {{{0}}}; //256*4k    => 1.0Mb
    RansDecSymbol32 syms[256][256+6] = {{{0}}}; //256*262*8 => 0.5Mb
#endif
#endif
    int16_t map[256], map_i = 0;
    
    memset(map, -1, 256*sizeof(*map));

    if (in_size < 27) // Need at least this many bytes to start
        return NULL;

    if (*in++ != 1) // Order-1 check
	return NULL;

    in_sz  = ((in[0])<<0) | ((in[1])<<8) | ((in[2])<<16) | ((in[3])<<24);
    out_sz = ((in[4])<<0) | ((in[5])<<8) | ((in[6])<<16) | ((in[7])<<24);
    if (in_sz != in_size-9)
	return NULL;

    if (out_sz >= INT_MAX)
	return NULL; // protect against some overflow cases

    // For speeding up the fuzzer only.
    // Small input can lead to large uncompressed data.
    // We reject this as it just slows things up instead of testing more code
    // paths (once we've verified a few times for large data).
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (out_sz > 100000)
	return NULL;
#endif

#if defined(USE_HEAP)
    if (!D || !syms) goto cleanup;
    /* These memsets prevent illegal memory access in syms due to
       broken compressed data.  As D is calloc'd, all illegal transitions
       will end up in either row or column 0 of syms. */
    memset(&syms[0], 0, sizeof(syms[0]));
    for (i = 0; i < 256; i++)
	memset(&syms[i][0], 0, sizeof(syms[0][0]));
#endif

    //fprintf(stderr, "out_sz=%d\n", out_sz);

    //i = *cp++;
    rle_i = 0;
    i = *cp++;
    do {
	// Map arbitrary a,b,c to 0,1,2 to improve cache locality.
	if (map[i] == -1)
	    map[i] = map_i++;
	int m_i = map[i];

	rle_j = x = 0;
	j = *cp++;
	do {
	    if (map[j] == -1)
		map[j] = map_i++;

	    int F, C;
            if (cp > ptr_end - 16) goto cleanup; // Not enough input bytes left
	    if ((F = *cp++) >= 128) {
		F &= ~128;
		F = ((F & 127) << 8) | *cp++;
	    }
	    C = x;

	    //fprintf(stderr, "i=%d j=%d F=%d C=%d\n", i, j, F, C);

	    if (!F)
		F = TOTFREQ;

	    RansDecSymbolInit32(&syms[m_i][j], C, F);

	    /* Build reverse lookup table */
	    //if (!D[i].R) D[i].R = (unsigned char *)malloc(TOTFREQ);
	    if (x + F > TOTFREQ)
		goto cleanup;

	    memset(&D[m_i].R[x], j, F);
	    x += F;

	    if (!rle_j && j+1 == *cp) {
		j = *cp++;
		rle_j = *cp++;
	    } else if (rle_j) {
		rle_j--;
		j++;
		if (j > 255)
		    goto cleanup;
	    } else {
		j = *cp++;
	    }
	} while(j);

        if (x < TOTFREQ-1 || x > TOTFREQ)
            goto cleanup;
        if (x < TOTFREQ) // historically we fill 4095, not 4096
            D[i].R[x] = D[i].R[x-1];

	if (!rle_i && i+1 == *cp) {
	    i = *cp++;
	    rle_i = *cp++;
	} else if (rle_i) {
	    rle_i--;
	    i++;
	    if (i > 255)
		goto cleanup;
	} else {
	    i = *cp++;
	}
    } while (i);
    for (i = 0; i < 256; i++)
	if (map[i] == -1)
	    map[i] = 0;

    RansState rans0, rans1, rans2, rans3;
    uint8_t *ptr = cp;
    if (cp > ptr_end - 16) goto cleanup; // Not enough input bytes left
    RansDecInit(&rans0, &ptr); if (rans0 < RANS_BYTE_L) return NULL;
    RansDecInit(&rans1, &ptr); if (rans1 < RANS_BYTE_L) return NULL;
    RansDecInit(&rans2, &ptr); if (rans2 < RANS_BYTE_L) return NULL;
    RansDecInit(&rans3, &ptr); if (rans3 < RANS_BYTE_L) return NULL;

    RansState R[4];
    R[0] = rans0;
    R[1] = rans1;
    R[2] = rans2;
    R[3] = rans3;

    unsigned int isz4 = out_sz>>2;
    uint32_t l0 = 0;
    uint32_t l1 = 0;
    uint32_t l2 = 0;
    uint32_t l3 = 0;
    
    unsigned int i4[] = {0*isz4, 1*isz4, 2*isz4, 3*isz4};

    /* Allocate output buffer */
    out_buf = malloc(out_sz);
    if (!out_buf) goto cleanup;

    uint8_t cc0 = D[map[l0]].R[R[0] & ((1u << TF_SHIFT)-1)];
    uint8_t cc1 = D[map[l1]].R[R[1] & ((1u << TF_SHIFT)-1)];
    uint8_t cc2 = D[map[l2]].R[R[2] & ((1u << TF_SHIFT)-1)];
    uint8_t cc3 = D[map[l3]].R[R[3] & ((1u << TF_SHIFT)-1)];

    ptr_end -= 8;
    for (; i4[0] < isz4; i4[0]++, i4[1]++, i4[2]++, i4[3]++) {
	out_buf[i4[0]] = cc0;
	out_buf[i4[1]] = cc1;
	out_buf[i4[2]] = cc2;
	out_buf[i4[3]] = cc3;

	//RansDecAdvanceStep(&R[0], syms[l0][cc0].start, syms[l0][cc0].freq, TF_SHIFT);
	//RansDecAdvanceStep(&R[1], syms[l1][cc1].start, syms[l1][cc1].freq, TF_SHIFT);
	//RansDecAdvanceStep(&R[2], syms[l2][cc2].start, syms[l2][cc2].freq, TF_vSHIFT);
	//RansDecAdvanceStep(&R[3], syms[l3][cc3].start, syms[l3][cc3].freq, TF_SHIFT);

	{
	    uint32_t m[4];

	    // Ordering to try and improve OoO cpu instructions
	    m[0] = R[0] & ((1u << TF_SHIFT)-1);
	    R[0] = syms[l0][cc0].freq * (R[0]>>TF_SHIFT);
	    m[1] = R[1] & ((1u << TF_SHIFT)-1);
	    R[0] += m[0] - syms[l0][cc0].start;
	    R[1] = syms[l1][cc1].freq * (R[1]>>TF_SHIFT);
	    m[2] = R[2] & ((1u << TF_SHIFT)-1);
	    R[1] += m[1] - syms[l1][cc1].start;
	    R[2] = syms[l2][cc2].freq * (R[2]>>TF_SHIFT);
	    m[3] = R[3] & ((1u << TF_SHIFT)-1);
	    R[3] = syms[l3][cc3].freq * (R[3]>>TF_SHIFT);
	    R[2] += m[2] - syms[l2][cc2].start;
	    R[3] += m[3] - syms[l3][cc3].start;
	}

	l0 = map[cc0];
	l1 = map[cc1];
	l2 = map[cc2];
	l3 = map[cc3];

	if (ptr < ptr_end) {
	    RansDecRenorm2(&R[0], &R[1], &ptr);
	    RansDecRenorm2(&R[2], &R[3], &ptr);
	} else {
	    RansDecRenormSafe(&R[0], &ptr, ptr_end+8);
	    RansDecRenormSafe(&R[1], &ptr, ptr_end+8);
	    RansDecRenormSafe(&R[2], &ptr, ptr_end+8);
	    RansDecRenormSafe(&R[3], &ptr, ptr_end+8);
	}

	cc0 = D[l0].R[R[0] & ((1u << TF_SHIFT)-1)];
	cc1 = D[l1].R[R[1] & ((1u << TF_SHIFT)-1)];
	cc2 = D[l2].R[R[2] & ((1u << TF_SHIFT)-1)];
	cc3 = D[l3].R[R[3] & ((1u << TF_SHIFT)-1)];
    }

    // Remainder
    for (; i4[3] < out_sz; i4[3]++) {
	unsigned char c3 = D[l3].R[RansDecGet(&R[3], TF_SHIFT)];
	out_buf[i4[3]] = c3;

	uint32_t m = R[3] & ((1u << TF_SHIFT)-1);
	R[3] = syms[l3][c3].freq * (R[3]>>TF_SHIFT) + m - syms[l3][c3].start;
	RansDecRenormSafe(&R[3], &ptr, ptr_end+8);
	l3 = map[c3];
    }
    
    *out_size = out_sz;

 cleanup:
#if defined(USE_HEAP)
    if (D)
        free(D);

    free(syms);
#endif

    return (unsigned char *)out_buf;
}

/*-----------------------------------------------------------------------------
 * Simple interface to the order-0 vs order-1 encoders and decoders.
 */
unsigned char *rans_compress(unsigned char *in, unsigned int in_size,
			     unsigned int *out_size, int order) {
    return order
	? rans_compress_O1(in, in_size, out_size)
	: rans_compress_O0(in, in_size, out_size);
}

unsigned char *rans_uncompress(unsigned char *in, unsigned int in_size,
			       unsigned int *out_size) {
    /* Both rans_uncompress functions need to be able to read at least 9
       bytes. */
    if (in_size < 9)
        return NULL;
    return in[0]
	? rans_uncompress_O1(in, in_size, out_size)
	: rans_uncompress_O0(in, in_size, out_size);
}
