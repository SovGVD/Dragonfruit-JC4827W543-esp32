#pragma once
#include "cybiko.h"

/* The MP3 player cartridge (CY45P01). Its SD card is the folder
 * /Cybiko/card: drop .mp3 files and MXD clips (.app) in it, up to 1 GB in
 * all. CyOS sees the folder as a FAT16 card; what CyOS does on the card
 * (deleting, writing, renaming files, making folders) is carried over to
 * the folder when the RAM disk is saved (cybiko_cart_sync).
 * With the folder empty, the FAT disk image /Cybiko/mp3card.img is used
 * instead (tools/cybiko_card.py makes one), and writes do reach it.
 * The music CyOS plays through the cartridge comes out of the speaker.
 * CyOS only uses the cartridge once the MP3 module software from the
 * cartridge's CD is on the RAM disk (unpack MP3.cap with tools/cybiko_cap.py
 * into /Cybiko/apps). Key in /Cybiko/cybiko.cfg:
 *   cart = auto    # default: plugged in when the card folder or image has files
 *   cart = mp3     # plugged in, with or without a card
 *   cart = off */

/* cfg is the text of /Cybiko/cybiko.cfg, or NULL. Call after cybiko_init
 * and before the machine runs. */
void cybiko_cart_init(cybiko_t *m, const char *cfg);
/* CPU task: pass the music CyOS sent to the decoder. */
void cybiko_cart_poll(cybiko_t *m);
/* Carry CyOS's changes on the card over to the card folder. Call with the
 * CPU task paused. */
void cybiko_cart_sync(void);
