// Berserk is a UCI compliant chess engine written in C
// Copyright (C) 2022 Jay Honnold

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__AVX512F__) || defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE__)
#include <xmmintrin.h>
#endif

#include "bits.h"
#include "board.h"
#include "nn.h"
#include "util.h"

#define INCBIN_PREFIX
#define INCBIN_STYLE INCBIN_STYLE_CAMEL
#include "incbin.h"

uint64_t NN_HASH = UINT64_MAX;

INCBIN(Embed, EVALFILE);

const int QUANTIZATION_PRECISION_IN = 16;
const int QUANTIZATION_PRECISION_OUT = 512;

int16_t INPUT_WEIGHTS[N_FEATURES * N_HIDDEN] __attribute__((aligned(ALIGN_ON)));
int16_t INPUT_BIASES[N_HIDDEN] __attribute__((aligned(ALIGN_ON)));
int16_t OUTPUT_WEIGHTS[2 * N_HIDDEN] __attribute__((aligned(ALIGN_ON)));
int32_t OUTPUT_BIAS;

INLINE void ApplyFeature(int16_t* dest, int16_t* src, int f, const int add) {
  for (int i = 0; i < N_HIDDEN; i++) dest[i] = src[i] + (2 * add - 1) * INPUT_WEIGHTS[f * N_HIDDEN + i];
}

int Predict(Board* board) {
  Accumulator stm, xstm;

  ResetAccumulator(stm, board, board->stm);
  ResetAccumulator(xstm, board, board->xstm);

  return OutputLayer(stm, xstm);
}

#if defined(__AVX512F__)
const size_t WIDTH = sizeof(__m512i) / sizeof(int16_t);
const size_t CHUNKS = N_HIDDEN / WIDTH;

int OutputLayer(Accumulator stmAccumulator, Accumulator xstmAccumulator) {
  int result = OUTPUT_BIAS;

  const __m512i zero = _mm512_setzero_si512();
  __m512i s0 = _mm512_setzero_si512();
  __m512i s1 = _mm512_setzero_si512();

  __m512i* stm = (__m512i*)stmAccumulator;
  __m512i* xstm = (__m512i*)xstmAccumulator;
  __m512i* weights = (__m512i*)OUTPUT_WEIGHTS;

  for (size_t j = 0; j < CHUNKS; j++) {
    const __m512i ac0 = _mm512_max_epi16(stm[j], zero);
    const __m512i ac1 = _mm512_max_epi16(xstm[j], zero);

    s0 = _mm512_add_epi32(s0, _mm512_madd_epi16(ac0, weights[j]));
    s1 = _mm512_add_epi32(s1, _mm512_madd_epi16(ac1, weights[j + CHUNKS]));
  }

  const __m512i r16 = _mm512_add_epi32(s0, s1);
  const __m256i r8 = _mm256_add_epi32(_mm512_castsi512_si256(r16), _mm512_extracti32x8_epi32(r16, 1));
  const __m128i r4 = _mm_add_epi32(_mm256_castsi256_si128(r8), _mm256_extractf128_si256(r8, 1));
  const __m128i r2 = _mm_add_epi32(r4, _mm_srli_si128(r4, 8));
  const __m128i r1 = _mm_add_epi32(r2, _mm_srli_si128(r2, 4));

  result += _mm_cvtsi128_si32(r1);
  return result / QUANTIZATION_PRECISION_IN / QUANTIZATION_PRECISION_OUT;
}
#elif defined(__AVX2__)
const size_t WIDTH = sizeof(__m256i) / sizeof(int16_t);
const size_t CHUNKS = N_HIDDEN / WIDTH;

int OutputLayer(Accumulator stmAccumulator, Accumulator xstmAccumulator) {
  int result = OUTPUT_BIAS;

  const __m256i zero = _mm256_setzero_si256();
  __m256i s0 = _mm256_setzero_si256();
  __m256i s1 = _mm256_setzero_si256();

  __m256i* stm = (__m256i*)stmAccumulator;
  __m256i* xstm = (__m256i*)xstmAccumulator;
  __m256i* weights = (__m256i*)OUTPUT_WEIGHTS;

  for (size_t j = 0; j < CHUNKS; j++) {
    const __m256i ac0 = _mm256_max_epi16(stm[j], zero);
    const __m256i ac1 = _mm256_max_epi16(xstm[j], zero);

    s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(ac0, weights[j]));
    s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(ac1, weights[j + CHUNKS]));
  }

  const __m256i r8 = _mm256_add_epi32(s0, s1);
  const __m128i r4 = _mm_add_epi32(_mm256_castsi256_si128(r8), _mm256_extractf128_si256(r8, 1));
  const __m128i r2 = _mm_add_epi32(r4, _mm_srli_si128(r4, 8));
  const __m128i r1 = _mm_add_epi32(r2, _mm_srli_si128(r2, 4));

  result += _mm_cvtsi128_si32(r1);
  return result / QUANTIZATION_PRECISION_IN / QUANTIZATION_PRECISION_OUT;
}
#elif defined(__SSE2__)
const size_t WIDTH = sizeof(__m128i) / sizeof(int16_t);
const size_t CHUNKS = N_HIDDEN / WIDTH;

int OutputLayer(Accumulator stmAccumulator, Accumulator xstmAccumulator) {
  int result = OUTPUT_BIAS;

  const __m128i zero = _mm_setzero_si128();
  __m128i s0 = _mm_setzero_si128();
  __m128i s1 = _mm_setzero_si128();

  __m128i* stm = (__m128i*)stmAccumulator;
  __m128i* xstm = (__m128i*)xstmAccumulator;
  __m128i* weights = (__m128i*)OUTPUT_WEIGHTS;

  for (size_t j = 0; j < N_HIDDEN / 8; j++) {
    const __m128i ac0 = _mm_max_epi16(stm[j], zero);
    const __m128i ac1 = _mm_max_epi16(xstm[j], zero);

    s0 = _mm_add_epi32(s0, _mm_madd_epi16(ac0, weights[j]));
    s1 = _mm_add_epi32(s1, _mm_madd_epi16(ac1, weights[j + CHUNKS]));
  }

  const __m128i r4 = _mm_add_epi32(s0, s1);
  const __m128i r2 = _mm_add_epi32(r4, _mm_srli_si128(r4, 8));
  const __m128i r1 = _mm_add_epi32(r2, _mm_srli_si128(r2, 4));

  result += _mm_cvtsi128_si32(r1);
  return result / QUANTIZATION_PRECISION_IN / QUANTIZATION_PRECISION_OUT;
}
#else
int OutputLayer(Accumulator stm, Accumulator xstm) {
  int result = OUTPUT_BIAS;

  for (int i = 0; i < N_HIDDEN; i++) {
    result += max(stm[i], 0) * OUTPUT_WEIGHTS[i];
    result += max(xstm[i], 0) * OUTPUT_WEIGHTS[i + N_HIDDEN];
  }

  return result / QUANTIZATION_PRECISION_IN / QUANTIZATION_PRECISION_OUT;
}
#endif

inline void ResetRefreshTable(Board* board) {
  for (int c = WHITE; c <= BLACK; c++) {
    for (int b = 0; b < 2 * N_KING_BUCKETS; b++) {
      AccumulatorKingState* state = &board->refreshTable[c][b];
      memcpy(state->values, INPUT_BIASES, sizeof(int16_t) * N_HIDDEN);
      memset(state->pcs, 0, sizeof(BitBoard) * 12);
    }
  }
}

inline void StoreAccumulatorKingState(Accumulator accumulator, Board* board, const int perspective) {
  int kingSq = lsb(PieceBB(KING, perspective));
  int kingBucket = KING_BUCKETS[kingSq ^ (56 * perspective)] + N_KING_BUCKETS * (File(kingSq) > 3);

  AccumulatorKingState* state = &board->refreshTable[perspective][kingBucket];

  memcpy(state->values, accumulator, sizeof(int16_t) * N_HIDDEN);
  for (int pc = WHITE_PAWN; pc <= BLACK_KING; pc++) state->pcs[pc] = board->pieces[pc];
}

// Refreshes an accumulator using a diff from the last known board state
// with proper king bucketing
inline void RefreshAccumulator(Accumulator accumulator, Board* board, const int perspective) {
  int kingSq = lsb(PieceBB(KING, perspective));
  int kingBucket = KING_BUCKETS[kingSq ^ (56 * perspective)] + N_KING_BUCKETS * (File(kingSq) > 3);

  AccumulatorKingState* state = &board->refreshTable[perspective][kingBucket];

  memcpy(accumulator, state->values, sizeof(int16_t) * N_HIDDEN);
  for (int pc = WHITE_PAWN; pc <= BLACK_KING; pc++) {
    BitBoard curr = board->pieces[pc];
    BitBoard prev = state->pcs[pc];

    BitBoard rem = prev & ~curr;
    BitBoard add = curr & ~prev;

    while (rem) {
      int sq = popAndGetLsb(&rem);
      ApplyFeature(accumulator, accumulator, FeatureIdx(pc, sq, kingSq, perspective), SUB);
    }

    while (add) {
      int sq = popAndGetLsb(&add);
      ApplyFeature(accumulator, accumulator, FeatureIdx(pc, sq, kingSq, perspective), ADD);
    }
  }

  StoreAccumulatorKingState(accumulator, board, perspective);
}

// Resets an accumulator from pieces on the board
inline void ResetAccumulator(Accumulator accumulator, Board* board, const int perspective) {
  int kingSq = lsb(PieceBB(KING, perspective));

  memcpy(accumulator, INPUT_BIASES, sizeof(Accumulator));

  BitBoard occ = OccBB(BOTH);
  while (occ) {
    int sq = popAndGetLsb(&occ);
    int pc = board->squares[sq];
    int feature = FeatureIdx(pc, sq, kingSq, perspective);

    for (int i = 0; i < N_HIDDEN; i++) accumulator[i] += INPUT_WEIGHTS[feature * N_HIDDEN + i];
  }
}

void ApplyUpdates(Board* board, int stm, NNUpdate* updates) {
  int16_t* output = board->accumulators[stm][board->acc];
  int16_t* prev = board->accumulators[stm][board->acc - 1];

  ApplyFeature(output, prev, updates->removals[0], SUB);
  for (int i = 1; i < updates->nr; i++) ApplyFeature(output, output, updates->removals[i], SUB);

  for (int i = 0; i < updates->na; i++) ApplyFeature(output, output, updates->additions[i], ADD);
}

const size_t NETWORK_SIZE = sizeof(int16_t) * N_FEATURES * N_HIDDEN +  // input weights
                            sizeof(int16_t) * N_HIDDEN +               // input biases
                            sizeof(int16_t) * 2 * N_HIDDEN +           // output weights
                            sizeof(int32_t);                           // output bias

INLINE void CopyData(const unsigned char* in) {
  size_t offset = 0;

  memcpy(INPUT_WEIGHTS, &in[offset], N_FEATURES * N_HIDDEN * sizeof(int16_t));
  offset += N_FEATURES * N_HIDDEN * sizeof(int16_t);
  memcpy(INPUT_BIASES, &in[offset], N_HIDDEN * sizeof(int16_t));
  offset += N_HIDDEN * sizeof(int16_t);
  memcpy(OUTPUT_WEIGHTS, &in[offset], 2 * N_HIDDEN * sizeof(int16_t));
  offset += 2 * N_HIDDEN * sizeof(int16_t);
  memcpy(&OUTPUT_BIAS, &in[offset], sizeof(int32_t));
}

void LoadDefaultNN() { CopyData(EmbedData); }

int LoadNetwork(char* path) {
  FILE* fin = fopen(path, "rb");
  if (fin == NULL) {
    printf("info string Unable to read file at %s\n", path);
    return 0;
  }

  uint8_t* data = malloc(NETWORK_SIZE);
  if (fread(data, sizeof(uint8_t), NETWORK_SIZE, fin) != NETWORK_SIZE) {
    printf("info string Error reading file at %s\n", path);
    return 0;
  }

  CopyData(data);

  fclose(fin);
  free(data);

  return 1;
}
