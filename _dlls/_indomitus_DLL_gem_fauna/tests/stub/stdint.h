/* Fake <stdint.h> for the host-side Windows-target syntax check: the host's glibc stdint.h assumes LP64 (long = 64 bit),
   which is wrong for x86_64-windows (LLP64), so take the widths from the compiler's target macros instead. */
#ifndef STUB_STDINT_H
#define STUB_STDINT_H
#define _BITS_STDINT_INTN_H 1   /* keep glibc headers (pulled in by stdio.h etc.) from redefining these */
#define _BITS_STDINT_UINTN_H 1
typedef __INT8_TYPE__ int8_t;     typedef __UINT8_TYPE__ uint8_t;
typedef __INT16_TYPE__ int16_t;   typedef __UINT16_TYPE__ uint16_t;
typedef __INT32_TYPE__ int32_t;   typedef __UINT32_TYPE__ uint32_t;
typedef __INT64_TYPE__ int64_t;   typedef __UINT64_TYPE__ uint64_t;
typedef __INTPTR_TYPE__ intptr_t; typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTMAX_TYPE__ intmax_t; typedef __UINTMAX_TYPE__ uintmax_t;
#endif
