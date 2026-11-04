// The port checks that baseoq4/pak0.pk4 is there, not its digest: reading it back would slow every start from
// the SD card, and tools/make_sd.py is what writes it. pak1.pk4 is not used on the Switch: see docs/architecture.md.
#ifndef OPENQ4_PAKS_GENERATED_H
#define OPENQ4_PAKS_GENERATED_H

#define OPENQ4_PAK0_MD5 ""
#define OPENQ4_PAK0_SIZE_BYTES 0
#define OPENQ4_PAK0_FILE_COUNT 0

#define OPENQ4_PAK1_MD5 ""
#define OPENQ4_PAK1_SIZE_BYTES 0
#define OPENQ4_PAK1_FILE_COUNT 0

#endif
