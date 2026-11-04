// Included before everything else in the build.
#ifndef SWITCH_COMPAT_H
#define SWITCH_COMPAT_H

#ifdef __cplusplus
#include <sys/types.h>
// newlib's BSD types define this as a macro, and openQ4's UI has fields with the name
#undef quad
#endif

#endif
