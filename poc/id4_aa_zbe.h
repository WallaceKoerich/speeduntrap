/*
 * Observe-only CCS / ZBE input for id4_aa_x86.
 * Copies E5.01 C10/C20 STATUS; HMI keeps the FBlock.
 */

#ifndef ID4_AA_ZBE_H
#define ID4_AA_ZBE_H

#include "id4_zbe.h"

int aa_zbe_start(void);
int aa_zbe_pop(Id4CcsEvent *ev);
/* NAV HighSync 0x33 via DSI ConnectionMaster, MOST 0x53 only as fallback. */
int aa_zbe_speech_hold(void);
void aa_zbe_speech_pump(void);   /* 60 ms DSI keep-alive on HighSync 0x36 */
void aa_zbe_speech_release(void);
extern volatile int g_zbe_pending;

#endif
