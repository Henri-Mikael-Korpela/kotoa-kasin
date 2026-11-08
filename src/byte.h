/**
 * @file byte.h
 * @brief The type for one byte of raw memory.
 */

#ifndef __KASIN_BYTE_H__
#define __KASIN_BYTE_H__

/**
 * One byte of raw memory. Unsigned so that values above 127 do not turn negative when widened.
 */
typedef unsigned char byte;

#endif  // __KASIN_BYTE_H__
