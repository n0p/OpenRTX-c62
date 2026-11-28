/* Stand-in for newlib's generated <math_config.h>.
 *
 * The fdlibm float sources only need two things: to know that the obsolete
 * (bit-twiddling, non correctly-rounded) implementations are the ones being
 * built, and newlib's private 32-bit type names, which the C library of the
 * Xtensa toolchain does not declare the way newlib's <sys/types.h> does.
 */
#ifndef _MATH_CONFIG_H
#define _MATH_CONFIG_H

#ifndef __OBSOLETE_MATH
#define __OBSOLETE_MATH 1
#endif

#include <stdint.h>
typedef int32_t  __int32_t;
typedef uint32_t __uint32_t;

#endif
