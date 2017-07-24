#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldm.h"

#define LDM_HASHTABLESIZE (1 << (LDM_MEMORY_USAGE))
#define LDM_HASH_ENTRY_SIZE_LOG 3
#define LDM_HASHTABLESIZE_U32 ((LDM_HASHTABLESIZE) >> 2)
#define LDM_HASHTABLESIZE_U64 ((LDM_HASHTABLESIZE) >> 3)

/* Hash table stuff. */
#define HASH_BUCKET_SIZE (1 << (HASH_BUCKET_SIZE_LOG))
#define LDM_HASHLOG ((LDM_MEMORY_USAGE)-(LDM_HASH_ENTRY_SIZE_LOG)-(HASH_BUCKET_SIZE_LOG))

#define COMPUTE_STATS
#define OUTPUT_CONFIGURATION
#define CHECKSUM_CHAR_OFFSET 10

// Take first match only.
//#define ZSTD_SKIP

//#define RUN_CHECKS

typedef U32 hash_t;

typedef struct LDM_hashEntry {
  U32 offset;
  U32 checksum;
} LDM_hashEntry;

struct LDM_compressStats {
  U32 windowSizeLog, hashTableSizeLog;
  U32 numMatches;
  U64 totalMatchLength;
  U64 totalLiteralLength;
  U64 totalOffset;

  U32 minOffset, maxOffset;

  U32 offsetHistogram[32];
};

typedef struct LDM_hashTable LDM_hashTable;

struct LDM_CCtx {
  U64 isize;             /* Input size */
  U64 maxOSize;          /* Maximum output size */

  const BYTE *ibase;        /* Base of input */
  const BYTE *ip;           /* Current input position */
  const BYTE *iend;         /* End of input */

  // Maximum input position such that hashing at the position does not exceed
  // end of input.
  const BYTE *ihashLimit;

  // Maximum input position such that finding a match of at least the minimum
  // match length does not exceed end of input.
  const BYTE *imatchLimit;

  const BYTE *obase;        /* Base of output */
  BYTE *op;                 /* Output */

  const BYTE *anchor;       /* Anchor to start of current (match) block */

  LDM_compressStats stats;            /* Compression statistics */

  LDM_hashTable *hashTable;

  const BYTE *lastPosHashed;          /* Last position hashed */
  hash_t lastHash;                    /* Hash corresponding to lastPosHashed */
  U32 lastSum;

  const BYTE *nextIp;                 // TODO: this is  redundant (ip + step)
  const BYTE *nextPosHashed;
  hash_t nextHash;                    /* Hash corresponding to nextPosHashed */
  U32 nextSum;

  unsigned step;                      // ip step, should be 1.

  const BYTE *lagIp;
  hash_t lagHash;
  U32 lagSum;

  U64 numHashInserts;
  // DEBUG
  const BYTE *DEBUG_setNextHash;
};

struct LDM_hashTable {
  U32 numBuckets;  // Number of buckets
  U32 numEntries;
  LDM_hashEntry *entries;

  BYTE *bucketOffsets;
};

/**
 * Create a hash table that can contain size elements.
 * The number of buckets is determined by size >> HASH_BUCKET_SIZE_LOG.
 */
LDM_hashTable *HASH_createTable(U32 size) {
  LDM_hashTable *table = malloc(sizeof(LDM_hashTable));
  table->numBuckets = size >> HASH_BUCKET_SIZE_LOG;
  table->numEntries = size;
  table->entries = calloc(size, sizeof(LDM_hashEntry));
  table->bucketOffsets = calloc(size >> HASH_BUCKET_SIZE_LOG, sizeof(BYTE));
  return table;
}

static LDM_hashEntry *getBucket(const LDM_hashTable *table, const hash_t hash) {
  return table->entries + (hash << HASH_BUCKET_SIZE_LOG);
}

static unsigned ZSTD_NbCommonBytes (register size_t val) {
    if (MEM_isLittleEndian()) {
        if (MEM_64bits()) {
#       if defined(_MSC_VER) && defined(_WIN64)
            unsigned long r = 0;
            _BitScanForward64( &r, (U64)val );
            return (unsigned)(r>>3);
#       elif defined(__GNUC__) && (__GNUC__ >= 3)
            return (__builtin_ctzll((U64)val) >> 3);
#       else
            static const int DeBruijnBytePos[64] = { 0, 0, 0, 0, 0, 1, 1, 2,
                                                     0, 3, 1, 3, 1, 4, 2, 7,
                                                     0, 2, 3, 6, 1, 5, 3, 5,
                                                     1, 3, 4, 4, 2, 5, 6, 7,
                                                     7, 0, 1, 2, 3, 3, 4, 6,
                                                     2, 6, 5, 5, 3, 4, 5, 6,
                                                     7, 1, 2, 4, 6, 4, 4, 5,
                                                     7, 2, 6, 5, 7, 6, 7, 7 };
            return DeBruijnBytePos[((U64)((val & -(long long)val) * 0x0218A392CDABBD3FULL)) >> 58];
#       endif
        } else { /* 32 bits */
#       if defined(_MSC_VER)
            unsigned long r=0;
            _BitScanForward( &r, (U32)val );
            return (unsigned)(r>>3);
#       elif defined(__GNUC__) && (__GNUC__ >= 3)
            return (__builtin_ctz((U32)val) >> 3);
#       else
            static const int DeBruijnBytePos[32] = { 0, 0, 3, 0, 3, 1, 3, 0,
                                                     3, 2, 2, 1, 3, 2, 0, 1,
                                                     3, 3, 1, 2, 2, 2, 2, 0,
                                                     3, 1, 2, 0, 1, 0, 1, 1 };
            return DeBruijnBytePos[((U32)((val & -(S32)val) * 0x077CB531U)) >> 27];
#       endif
        }
    } else {  /* Big Endian CPU */
        if (MEM_64bits()) {
#       if defined(_MSC_VER) && defined(_WIN64)
            unsigned long r = 0;
            _BitScanReverse64( &r, val );
            return (unsigned)(r>>3);
#       elif defined(__GNUC__) && (__GNUC__ >= 3)
            return (__builtin_clzll(val) >> 3);
#       else
            unsigned r;
            const unsigned n32 = sizeof(size_t)*4;   /* calculate this way due to compiler complaining in 32-bits mode */
            if (!(val>>n32)) { r=4; } else { r=0; val>>=n32; }
            if (!(val>>16)) { r+=2; val>>=8; } else { val>>=24; }
            r += (!val);
            return r;
#       endif
        } else { /* 32 bits */
#       if defined(_MSC_VER)
            unsigned long r = 0;
            _BitScanReverse( &r, (unsigned long)val );
            return (unsigned)(r>>3);
#       elif defined(__GNUC__) && (__GNUC__ >= 3)
            return (__builtin_clz((U32)val) >> 3);
#       else
            unsigned r;
            if (!(val>>16)) { r=2; val>>=8; } else { r=0; val>>=24; }
            r += (!val);
            return r;
#       endif
    }   }
}

// From lib/compress/zstd_compress.c
static size_t ZSTD_count(const BYTE *pIn, const BYTE *pMatch,
                         const BYTE *const pInLimit) {
    const BYTE * const pStart = pIn;
    const BYTE * const pInLoopLimit = pInLimit - (sizeof(size_t)-1);

    while (pIn < pInLoopLimit) {
        size_t const diff = MEM_readST(pMatch) ^ MEM_readST(pIn);
        if (!diff) {
          pIn += sizeof(size_t);
          pMatch += sizeof(size_t);
          continue;
        }
        pIn += ZSTD_NbCommonBytes(diff);
        return (size_t)(pIn - pStart);
    }

    if (MEM_64bits()) {
      if ((pIn < (pInLimit - 3)) && (MEM_read32(pMatch) == MEM_read32(pIn))) {
        pIn += 4;
        pMatch += 4;
      }
    }
    if ((pIn < (pInLimit - 1)) && (MEM_read16(pMatch) == MEM_read16(pIn))) {
      pIn += 2;
      pMatch += 2;
    }
    if ((pIn < pInLimit) && (*pMatch == *pIn)) {
      pIn++;
    }
    return (size_t)(pIn - pStart);
}

/**
 * Count number of bytes that match backwards before pIn and pMatch.
 *
 * We count only bytes where pMatch > pBaes and pIn > pAnchor.
 */
U32 countBackwardsMatch(const BYTE *pIn, const BYTE *pAnchor,
                        const BYTE *pMatch, const BYTE *pBase) {
  U32 matchLength = 0;
  while (pIn > pAnchor && pMatch > pBase && pIn[-1] == pMatch[-1]) {
    pIn--;
    pMatch--;
    matchLength++;
  }
  return matchLength;
}

/**
 * Returns a pointer to the entry in the hash table matching the hash and
 * checksum with the "longest match length" as defined below. The forward and
 * backward match lengths are written to *pForwardMatchLength and
 * *pBackwardMatchLength.
 *
 * The match length is defined based on cctx->ip and the entry's offset.
 * The forward match is computed from cctx->ip and entry->offset + cctx->ibase.
 * The backward match is computed backwards from cctx->ip and
 * cctx->ibase only if the forward match is longer than LDM_MIN_MATCH_LENGTH.
 *
 */
LDM_hashEntry *HASH_getBestEntry(const LDM_CCtx *cctx,
                                 const hash_t hash,
                                 const U32 checksum,
                                 U32 *pForwardMatchLength,
                                 U32 *pBackwardMatchLength) {
  LDM_hashTable *table = cctx->hashTable;
  LDM_hashEntry *bucket = getBucket(table, hash);
  LDM_hashEntry *cur = bucket;
  LDM_hashEntry *bestEntry = NULL;
  U32 bestMatchLength = 0;
  for (; cur < bucket + HASH_BUCKET_SIZE; ++cur) {
    const BYTE *pMatch = cur->offset + cctx->ibase;

    // Check checksum for faster check.
    if (cur->checksum == checksum &&
        cctx->ip - pMatch <= LDM_WINDOW_SIZE) {
      U32 forwardMatchLength = ZSTD_count(cctx->ip, pMatch, cctx->iend);
      U32 backwardMatchLength, totalMatchLength;

      // For speed.
      if (forwardMatchLength < LDM_MIN_MATCH_LENGTH) {
        continue;
      }

      backwardMatchLength =
          countBackwardsMatch(cctx->ip, cctx->anchor,
                              cur->offset + cctx->ibase,
                              cctx->ibase);

      totalMatchLength = forwardMatchLength + backwardMatchLength;

      if (totalMatchLength >= bestMatchLength) {
        bestMatchLength = totalMatchLength;
        *pForwardMatchLength = forwardMatchLength;
        *pBackwardMatchLength = backwardMatchLength;

        bestEntry = cur;
#ifdef ZSTD_SKIP
        return cur;
#endif
      }
    }
  }
  if (bestEntry != NULL) {
    return bestEntry;
  }
  return NULL;
}

void HASH_insert(LDM_hashTable *table,
                 const hash_t hash, const LDM_hashEntry entry) {
  *(getBucket(table, hash) + table->bucketOffsets[hash]) = entry;
  table->bucketOffsets[hash]++;
  table->bucketOffsets[hash] &= HASH_BUCKET_SIZE - 1;
}

U32 HASH_getSize(const LDM_hashTable *table) {
  return table->numBuckets;
}

void HASH_destroyTable(LDM_hashTable *table) {
  free(table->entries);
  free(table->bucketOffsets);
  free(table);
}

void HASH_outputTableOccupancy(const LDM_hashTable *table) {
  U32 ctr = 0;
  LDM_hashEntry *cur = table->entries;
  LDM_hashEntry *end = table->entries + (table->numBuckets * HASH_BUCKET_SIZE);
  for (; cur < end; ++cur) {
    if (cur->offset == 0) {
      ctr++;
    }
  }

  printf("Num buckets, bucket size: %d, %d\n",
         table->numBuckets, HASH_BUCKET_SIZE);
  printf("Hash table size, empty slots, %% empty: %u, %u, %.3f\n",
         table->numEntries, ctr,
         100.0 * (double)(ctr) / table->numEntries);
}

// TODO: This can be done more efficiently (but it is not that important as it
// is only used for computing stats).
static int intLog2(U32 x) {
  int ret = 0;
  while (x >>= 1) {
    ret++;
  }
  return ret;
}

void LDM_printCompressStats(const LDM_compressStats *stats) {
  int i = 0;
  printf("=====================\n");
  printf("Compression statistics\n");
  printf("Window size, hash table size (bytes): 2^%u, 2^%u\n",
          stats->windowSizeLog, stats->hashTableSizeLog);
  printf("num matches, total match length, %% matched: %u, %llu, %.3f\n",
          stats->numMatches,
          stats->totalMatchLength,
          100.0 * (double)stats->totalMatchLength /
              (double)(stats->totalMatchLength + stats->totalLiteralLength));
  printf("avg match length: %.1f\n", ((double)stats->totalMatchLength) /
                                         (double)stats->numMatches);
  printf("avg literal length, total literalLength: %.1f, %llu\n",
         ((double)stats->totalLiteralLength) / (double)stats->numMatches,
         stats->totalLiteralLength);
  printf("avg offset length: %.1f\n",
         ((double)stats->totalOffset) / (double)stats->numMatches);
  printf("min offset, max offset: %u, %u\n",
         stats->minOffset, stats->maxOffset);

  printf("\n");
  printf("offset histogram: offset, num matches, %% of matches\n");

  for (; i <= intLog2(stats->maxOffset); i++) {
    printf("2^%*d: %10u    %6.3f%%\n", 2, i,
           stats->offsetHistogram[i],
           100.0 * (double) stats->offsetHistogram[i] /
                   (double) stats->numMatches);
  }
  printf("\n");
  printf("=====================\n");
}

int LDM_isValidMatch(const BYTE *pIn, const BYTE *pMatch) {
  U32 lengthLeft = LDM_MIN_MATCH_LENGTH;
  const BYTE *curIn = pIn;
  const BYTE *curMatch = pMatch;

  if (pIn - pMatch > LDM_WINDOW_SIZE) {
    return 0;
  }

  for (; lengthLeft >= 4; lengthLeft -= 4) {
    if (MEM_read32(curIn) != MEM_read32(curMatch)) {
      return 0;
    }
    curIn += 4;
    curMatch += 4;
  }
  return 1;
}

hash_t HASH_hashU32(U32 value) {
  return ((value * 2654435761U) >> (32 - LDM_HASHLOG));
}

/**
 * Convert a sum computed from getChecksum to a hash value in the range
 * of the hash table.
 */
static hash_t checksumToHash(U32 sum) {
  return HASH_hashU32(sum);
}

/**
 * Computes a checksum based on rsync's checksum.
 *
 * a(k,l) = \sum_{i = k}^l x_i (mod M)
 * b(k,l) = \sum_{i = k}^l ((l - i + 1) * x_i) (mod M)
 * checksum(k,l) = a(k,l) + 2^{16} * b(k,l)
  */
static U32 getChecksum(const BYTE *buf, U32 len) {
  U32 i;
  U32 s1, s2;

  s1 = s2 = 0;
  for (i = 0; i < (len - 4); i += 4) {
    s2 += (4 * (s1 + buf[i])) + (3 * buf[i + 1]) +
          (2 * buf[i + 2]) + (buf[i + 3]) +
          (10 * CHECKSUM_CHAR_OFFSET);
    s1 += buf[i] + buf[i + 1] + buf[i + 2] + buf[i + 3] +
          + (4 * CHECKSUM_CHAR_OFFSET);

  }
  for(; i < len; i++) {
    s1 += buf[i] + CHECKSUM_CHAR_OFFSET;
    s2 += s1;
  }
  return (s1 & 0xffff) + (s2 << 16);
}

/**
 * Update a checksum computed from getChecksum(data, len).
 *
 * The checksum can be updated along its ends as follows:
 * a(k+1, l+1) = (a(k,l) - x_k + x_{l+1}) (mod M)
 * b(k+1, l+1) = (b(k,l) - (l-k+1)*x_k + (a(k+1,l+1)) (mod M)
 *
 * Thus toRemove should correspond to data[0].
 */
static U32 updateChecksum(U32 sum, U32 len,
                          BYTE toRemove, BYTE toAdd) {
  U32 s1 = (sum & 0xffff) - toRemove + toAdd;
  U32 s2 = (sum >> 16) - ((toRemove + CHECKSUM_CHAR_OFFSET) * len) + s1;

  return (s1 & 0xffff) + (s2 << 16);
}

/**
 * Update cctx->nextSum, cctx->nextHash, and cctx->nextPosHashed
 * based on cctx->lastSum and cctx->lastPosHashed.
 *
 * This uses a rolling hash and requires that the last position hashed
 * corresponds to cctx->nextIp - step.
 */
static void setNextHash(LDM_CCtx *cctx) {
#ifdef RUN_CHECKS
  U32 check;
  if ((cctx->nextIp - cctx->ibase != 1) &&
      (cctx->nextIp - cctx->DEBUG_setNextHash != 1)) {
    printf("CHECK debug fail: %zu %zu\n", cctx->nextIp - cctx->ibase,
            cctx->DEBUG_setNextHash - cctx->ibase);
  }

  cctx->DEBUG_setNextHash = cctx->nextIp;
#endif

  cctx->nextSum = updateChecksum(
      cctx->lastSum, LDM_HASH_LENGTH,
      cctx->lastPosHashed[0],
      cctx->lastPosHashed[LDM_HASH_LENGTH]);
  cctx->nextPosHashed = cctx->nextIp;
  cctx->nextHash = checksumToHash(cctx->nextSum);

#if LDM_LAG
  if (cctx->ip - cctx->ibase > LDM_LAG) {
    cctx->lagSum = updateChecksum(
      cctx->lagSum, LDM_HASH_LENGTH,
      cctx->lagIp[0], cctx->lagIp[LDM_HASH_LENGTH]);
    cctx->lagIp++;
    cctx->lagHash = checksumToHash(cctx->lagSum);
  }
#endif

#ifdef RUN_CHECKS
  check = getChecksum(cctx->nextIp, LDM_HASH_LENGTH);

  if (check != cctx->nextSum) {
    printf("CHECK: setNextHash failed %u %u\n", check, cctx->nextSum);
  }

  if ((cctx->nextIp - cctx->lastPosHashed) != 1) {
    printf("setNextHash: nextIp != lastPosHashed + 1. %zu %zu %zu\n",
            cctx->nextIp - cctx->ibase, cctx->lastPosHashed - cctx->ibase,
            cctx->ip - cctx->ibase);
  }
#endif
}

static void putHashOfCurrentPositionFromHash(
    LDM_CCtx *cctx, hash_t hash, U32 sum) {
  // Hash only every HASH_ONLY_EVERY times, based on cctx->ip.
  // Note: this works only when cctx->step is 1.
  if (((cctx->ip - cctx->ibase) & HASH_ONLY_EVERY) == HASH_ONLY_EVERY) {
#if LDM_LAG
    // TODO: off by 1, but whatever
    if (cctx->lagIp - cctx->ibase > 0) {
      const LDM_hashEntry entry = { cctx->lagIp - cctx->ibase, cctx->lagSum };
      HASH_insert(cctx->hashTable, cctx->lagHash, entry);
    } else {
      const LDM_hashEntry entry = { cctx->ip - cctx->ibase, sum };
      HASH_insert(cctx->hashTable, hash, entry);
    }
#else
    const LDM_hashEntry entry = { cctx->ip - cctx->ibase, sum };
    HASH_insert(cctx->hashTable, hash, entry);
#endif
  }

  cctx->lastPosHashed = cctx->ip;
  cctx->lastHash = hash;
  cctx->lastSum = sum;
}

/**
 * Copy over the cctx->lastHash, cctx->lastSum, and cctx->lastPosHashed
 * fields from the "next" fields.
 *
 * This requires that cctx->ip == cctx->nextPosHashed.
 */
static void LDM_updateLastHashFromNextHash(LDM_CCtx *cctx) {
#ifdef RUN_CHECKS
  if (cctx->ip != cctx->nextPosHashed) {
    printf("CHECK failed: updateLastHashFromNextHash %zu\n",
           cctx->ip - cctx->ibase);
  }
#endif
  putHashOfCurrentPositionFromHash(cctx, cctx->nextHash, cctx->nextSum);
}

/**
 * Insert hash of the current position into the hash table.
 */
static void LDM_putHashOfCurrentPosition(LDM_CCtx *cctx) {
  U32 sum = getChecksum(cctx->ip, LDM_HASH_LENGTH);
  hash_t hash = checksumToHash(sum);

#ifdef RUN_CHECKS
  if (cctx->nextPosHashed != cctx->ip && (cctx->ip != cctx->ibase)) {
    printf("CHECK failed: putHashOfCurrentPosition %zu\n",
           cctx->ip - cctx->ibase);
  }
#endif

  putHashOfCurrentPositionFromHash(cctx, hash, sum);
}

void LDM_initializeCCtx(LDM_CCtx *cctx,
                        const void *src, size_t srcSize,
                        void *dst, size_t maxDstSize) {
  cctx->isize = srcSize;
  cctx->maxOSize = maxDstSize;

  cctx->ibase = (const BYTE *)src;
  cctx->ip = cctx->ibase;
  cctx->iend = cctx->ibase + srcSize;

  cctx->ihashLimit = cctx->iend - LDM_HASH_LENGTH;
  cctx->imatchLimit = cctx->iend - LDM_MIN_MATCH_LENGTH;

  cctx->obase = (BYTE *)dst;
  cctx->op = (BYTE *)dst;

  cctx->anchor = cctx->ibase;

  memset(&(cctx->stats), 0, sizeof(cctx->stats));
  cctx->hashTable = HASH_createTable(LDM_HASHTABLESIZE_U64);

  cctx->stats.minOffset = UINT_MAX;
  cctx->stats.windowSizeLog = LDM_WINDOW_SIZE_LOG;
  cctx->stats.hashTableSizeLog = LDM_MEMORY_USAGE;


  cctx->lastPosHashed = NULL;

  cctx->step = 1;   // Fixed to be 1 for now. Changing may break things.
  cctx->nextIp = cctx->ip + cctx->step;
  cctx->nextPosHashed = 0;

  cctx->DEBUG_setNextHash = 0;
}

void LDM_destroyCCtx(LDM_CCtx *cctx) {
  HASH_destroyTable(cctx->hashTable);
}

/**
 * Finds the "best" match.
 *
 * Returns 0 if successful and 1 otherwise (i.e. no match can be found
 * in the remaining input that is long enough).
 *
 * forwardMatchLength contains the forward length of the match.
 */
static int LDM_findBestMatch(LDM_CCtx *cctx, const BYTE **match,
                             U32 *forwardMatchLength, U32 *backwardMatchLength) {

  LDM_hashEntry *entry = NULL;
  cctx->nextIp = cctx->ip + cctx->step;

  while (entry == NULL) {
    hash_t h;
    U32 sum;
    setNextHash(cctx);
    h = cctx->nextHash;
    sum = cctx->nextSum;
    cctx->ip = cctx->nextIp;
    cctx->nextIp += cctx->step;

    if (cctx->ip > cctx->imatchLimit) {
      return 1;
    }

    entry = HASH_getBestEntry(cctx, h, sum,
                              forwardMatchLength, backwardMatchLength);

    if (entry != NULL) {
      *match = entry->offset + cctx->ibase;
    }
    putHashOfCurrentPositionFromHash(cctx, h, sum);
  }
  setNextHash(cctx);
  return 0;
}

void LDM_encodeLiteralLengthAndLiterals(
    LDM_CCtx *cctx, BYTE *pToken, const U64 literalLength) {
  /* Encode the literal length. */
  if (literalLength >= RUN_MASK) {
    U64 len = (U64)literalLength - RUN_MASK;
    *pToken = (RUN_MASK << ML_BITS);
    for (; len >= 255; len -= 255) {
      *(cctx->op)++ = 255;
    }
    *(cctx->op)++ = (BYTE)len;
  } else {
    *pToken = (BYTE)(literalLength << ML_BITS);
  }

  /* Encode the literals. */
  memcpy(cctx->op, cctx->anchor, literalLength);
  cctx->op += literalLength;
}

void LDM_outputBlock(LDM_CCtx *cctx,
                     const U64 literalLength,
                     const U32 offset,
                     const U64 matchLength) {
  BYTE *pToken = cctx->op++;

  /* Encode the literal length and literals. */
  LDM_encodeLiteralLengthAndLiterals(cctx, pToken, literalLength);

  /* Encode the offset. */
  MEM_write32(cctx->op, offset);
  cctx->op += LDM_OFFSET_SIZE;

  /* Encode the match length. */
  if (matchLength >= ML_MASK) {
    unsigned matchLengthRemaining = matchLength;
    *pToken += ML_MASK;
    matchLengthRemaining -= ML_MASK;
    MEM_write32(cctx->op, 0xFFFFFFFF);
    while (matchLengthRemaining >= 4*0xFF) {
      cctx->op += 4;
      MEM_write32(cctx->op, 0xffffffff);
      matchLengthRemaining -= 4*0xFF;
    }
    cctx->op += matchLengthRemaining / 255;
    *(cctx->op)++ = (BYTE)(matchLengthRemaining % 255);
  } else {
    *pToken += (BYTE)(matchLength);
  }
}

// TODO: maxDstSize is unused. This function may seg fault when writing
// beyond the size of dst, as it does not check maxDstSize. Writing to
// a buffer and performing checks is a possible solution.
//
// This is based upon lz4.
size_t LDM_compress(const void *src, size_t srcSize,
                    void *dst, size_t maxDstSize) {
  LDM_CCtx cctx;
  const BYTE *match = NULL;
  U32 forwardMatchLength = 0;
  U32 backwardsMatchLength = 0;

  LDM_initializeCCtx(&cctx, src, srcSize, dst, maxDstSize);
  LDM_outputConfiguration();

  /* Hash the first position and put it into the hash table. */
  LDM_putHashOfCurrentPosition(&cctx);

#if LDM_LAG
  cctx.lagIp = cctx.ip;
  cctx.lagHash = cctx.lastHash;
  cctx.lagSum = cctx.lastSum;
#endif
  /**
   * Find a match.
   * If no more matches can be found (i.e. the length of the remaining input
   * is less than the minimum match length), then stop searching for matches
   * and encode the final literals.
   */
  while (LDM_findBestMatch(&cctx, &match, &forwardMatchLength,
         &backwardsMatchLength) == 0) {
#ifdef COMPUTE_STATS
    cctx.stats.numMatches++;
#endif

     cctx.ip -= backwardsMatchLength;
     match -= backwardsMatchLength;

    /**
     * Write current block (literals, literal length, match offset, match
     * length) and update pointers and hashes.
     */
    {
      const U64 literalLength = cctx.ip - cctx.anchor;
      const U32 offset = cctx.ip - match;
      const U64 matchLength = forwardMatchLength +
                              backwardsMatchLength -
                              LDM_MIN_MATCH_LENGTH;

      LDM_outputBlock(&cctx, literalLength, offset, matchLength);

#ifdef COMPUTE_STATS
      cctx.stats.totalLiteralLength += literalLength;
      cctx.stats.totalOffset += offset;
      cctx.stats.totalMatchLength += matchLength + LDM_MIN_MATCH_LENGTH;
      cctx.stats.minOffset =
          offset < cctx.stats.minOffset ? offset : cctx.stats.minOffset;
      cctx.stats.maxOffset =
          offset > cctx.stats.maxOffset ? offset : cctx.stats.maxOffset;
      cctx.stats.offsetHistogram[(U32)intLog2(offset)]++;
#endif

      // Move ip to end of block, inserting hashes at each position.
      cctx.nextIp = cctx.ip + cctx.step;
      while (cctx.ip < cctx.anchor + LDM_MIN_MATCH_LENGTH +
                       matchLength + literalLength) {
        if (cctx.ip > cctx.lastPosHashed) {
          // TODO: Simplify.
          LDM_updateLastHashFromNextHash(&cctx);
          setNextHash(&cctx);
        }
        cctx.ip++;
        cctx.nextIp++;
      }
    }

    // Set start of next block to current input pointer.
    cctx.anchor = cctx.ip;
    LDM_updateLastHashFromNextHash(&cctx);
  }

  // HASH_outputTableOffsetHistogram(&cctx);

  /* Encode the last literals (no more matches). */
  {
    const U32 lastRun = cctx.iend - cctx.anchor;
    BYTE *pToken = cctx.op++;
    LDM_encodeLiteralLengthAndLiterals(&cctx, pToken, lastRun);
  }

#ifdef COMPUTE_STATS
  LDM_printCompressStats(&cctx.stats);
  HASH_outputTableOccupancy(cctx.hashTable);
#endif

  {
    const size_t ret = cctx.op - cctx.obase;
    LDM_destroyCCtx(&cctx);
    return ret;
  }
}

// TODO: implement and test hash function
void LDM_test(const BYTE *src) {
  (void)src;
}


