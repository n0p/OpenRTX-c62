/* C wrapper around the reference CGolay24128 (not part of the delivery). */
#include "cgolay24128.h"
extern "C" unsigned int ref_decode24128(unsigned int code) { return CGolay24128::decode24128(code); }
extern "C" unsigned int ref_decode23127(unsigned int code) { return CGolay24128::decode23127(code); }
