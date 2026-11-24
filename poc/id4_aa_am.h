#ifndef ID4_AA_AM_H
#define ID4_AA_AM_H

/* AudioManagement DSI — unmute NAV source 0x56 after CM builds 0x33. */
int aa_am_start(void);
int aa_am_ready(void);
int aa_am_request_focus(void);
void aa_am_release_focus(void);

#endif
