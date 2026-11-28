/* Stand-in for newlib's <machine/ieeefp.h>, which is generated per target by
 * newlib's configure.  Both targets of this port (Cortex-M33 and the HiFi4) are
 * little-endian, IEEE-754 with denormals, and are configured with FLT_EVAL_
 * METHOD 0, which is all fdlibm needs to know. */
#ifndef _MACHINE_IEEEFP_H
#define _MACHINE_IEEEFP_H
#ifndef __IEEE_LITTLE_ENDIAN
#define __IEEE_LITTLE_ENDIAN 1
#endif
#endif
