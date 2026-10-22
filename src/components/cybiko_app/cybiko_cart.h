#pragma once
#include "cybiko.h"

/* The MP3 player cartridge (CY45P01). Its SD card is the FAT disk image
 * /Cybiko/mp3card.img (tools/cybiko_card.py makes one from a folder), and
 * the music CyOS plays through it comes out of the speaker. CyOS only uses
 * the cartridge once the MP3 module software from the cartridge's CD is on
 * the RAM disk (unpack MP3.cap with tools/cybiko_cap.py into /Cybiko/apps).
 * Key in /Cybiko/cybiko.cfg:
 *   cart = auto    # default: plugged in when mp3card.img exists
 *   cart = mp3     # plugged in, with or without a card
 *   cart = off */

/* cfg is the text of /Cybiko/cybiko.cfg, or NULL. Call after cybiko_init
 * and before the machine runs. */
void cybiko_cart_init(cybiko_t *m, const char *cfg);
/* CPU task: pass the music CyOS sent to the decoder. */
void cybiko_cart_poll(cybiko_t *m);
