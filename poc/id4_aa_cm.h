#ifndef ID4_AA_CM_H
#define ID4_AA_CM_H

#include <stdint.h>

/* ConnectionMaster.DMMNBTConnectionMaster — OEM HighSync, not foreign 0x53. */
int aa_cm_start(void);
int aa_cm_ready(void);
int aa_cm_build(uint16_t conn);
void aa_cm_remove(void);

#endif
