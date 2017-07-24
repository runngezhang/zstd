#include <stdio.h>

#include "ldm.h"

void LDM_outputConfiguration(void) {
  printf("=====================\n");
  printf("Configuration\n");
  printf("LDM_WINDOW_SIZE_LOG: %d\n", LDM_WINDOW_SIZE_LOG);
  printf("LDM_MIN_MATCH_LENGTH, LDM_HASH_LENGTH: %d, %d\n",
         LDM_MIN_MATCH_LENGTH, LDM_HASH_LENGTH);
  printf("LDM_MEMORY_USAGE: %d\n", LDM_MEMORY_USAGE);
  printf("HASH_ONLY_EVERY_LOG: %d\n", HASH_ONLY_EVERY_LOG);
  printf("HASH_BUCKET_SIZE_LOG: %d\n", HASH_BUCKET_SIZE_LOG);
  printf("LDM_LAG %d\n", LDM_LAG);
  printf("=====================\n");
}

void LDM_readHeader(const void *src, U64 *compressedSize,
                    U64 *decompressedSize) {
  const BYTE *ip = (const BYTE *)src;
  *compressedSize = MEM_readLE64(ip);
  ip += sizeof(U64);
  *decompressedSize = MEM_readLE64(ip);
  // ip += sizeof(U64);
}

void LDM_writeHeader(void *memPtr, U64 compressedSize,
                     U64 decompressedSize) {
  MEM_write64(memPtr, compressedSize);
  MEM_write64((BYTE *)memPtr + 8, decompressedSize);
}

struct LDM_DCtx {
  size_t compressedSize;
  size_t maxDecompressedSize;

  const BYTE *ibase;   /* Base of input */
  const BYTE *ip;      /* Current input position */
  const BYTE *iend;    /* End of source */

  const BYTE *obase;   /* Base of output */
  BYTE *op;            /* Current output position */
  const BYTE *oend;    /* End of output */
};

void LDM_initializeDCtx(LDM_DCtx *dctx,
                        const void *src, size_t compressedSize,
                        void *dst, size_t maxDecompressedSize) {
  dctx->compressedSize = compressedSize;
  dctx->maxDecompressedSize = maxDecompressedSize;

  dctx->ibase = src;
  dctx->ip = (const BYTE *)src;
  dctx->iend = dctx->ip + dctx->compressedSize;
  dctx->op = dst;
  dctx->oend = dctx->op + dctx->maxDecompressedSize;
}

size_t LDM_decompress(const void *src, size_t compressedSize,
                      void *dst, size_t maxDecompressedSize) {

  LDM_DCtx dctx;
  LDM_initializeDCtx(&dctx, src, compressedSize, dst, maxDecompressedSize);

  while (dctx.ip < dctx.iend) {
    BYTE *cpy;
    const BYTE *match;
    size_t length, offset;

    /* Get the literal length. */
    const unsigned token = *(dctx.ip)++;
    if ((length = (token >> ML_BITS)) == RUN_MASK) {
      unsigned s;
      do {
        s = *(dctx.ip)++;
        length += s;
      } while (s == 255);
    }

    /* Copy the literals. */
    cpy = dctx.op + length;
    memcpy(dctx.op, dctx.ip, length);
    dctx.ip += length;
    dctx.op = cpy;

    //TODO : dynamic offset size
    offset = MEM_read32(dctx.ip);
    dctx.ip += LDM_OFFSET_SIZE;
    match = dctx.op - offset;

    /* Get the match length. */
    length = token & ML_MASK;
    if (length == ML_MASK) {
      unsigned s;
      do {
        s = *(dctx.ip)++;
        length += s;
      } while (s == 255);
    }
    length += LDM_MIN_MATCH_LENGTH;

    /* Copy match. */
    cpy = dctx.op + length;

    // Inefficient for now.
    while (match < cpy - offset && dctx.op < dctx.oend) {
      *(dctx.op)++ = *match++;
    }
  }
  return dctx.op - (BYTE *)dst;
}


