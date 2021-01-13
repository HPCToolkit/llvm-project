#ifndef OMPT_GENERAL_H
#define OMPT_GENERAL_H

#if OMPT_DEBUG
#include <stdio.h>
#define DEBUGP(prefix, ...)                                                    \
  {                                                                            \
    fprintf(stderr, "%s --> ", prefix);                                        \
    fprintf(stderr, __VA_ARGS__);                                              \
  }

#include <inttypes.h>
#define DPxMOD "0x%0*" PRIxPTR
#define DPxPTR(ptr) ((int)(2*sizeof(uintptr_t))), ((uintptr_t) (ptr))

/*
 * To printf a pointer in hex with a fixed width of 16 digits and a leading 0x,
 * use printf("ptr=" DPxMOD "...\n", DPxPTR(ptr));
 *
 * DPxMOD expands to:
 *   "0x%0*" PRIxPTR
 * where PRIxPTR expands to an appropriate modifier for the type uintptr_t on a
 * specific platform, e.g. "lu" if uintptr_t is typedef'd as unsigned long:
 *   "0x%0*lu"
 *
 * Ultimately, the whole statement expands to:
 *   printf("ptr=0x%0*lu...\n",  // the 0* modifier expects an extra argument
 *                               // specifying the width of the output
 *   (int)(2*sizeof(uintptr_t)), // the extra argument specifying the width
 *                               // 8 digits for 32bit systems
 *                               // 16 digits for 64bit
 *   (uintptr_t) ptr);
 */
#else
#define DEBUGP(prefix, ...)                                                    \
  {}
#endif

#define DP(...) DEBUGP("libomp", __VA_ARGS__)

#endif /* OMPT_GENERAL_H */
