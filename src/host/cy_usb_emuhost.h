#pragma once
/* A USB host for the emulated Cybiko (cy_usb_emuhost.c). */
#include <stdbool.h>
#include "cybiko.h"
#include "cyusb.h"

typedef struct cy_usbhc cy_usbhc_t;
cy_usbhc_t *cy_usbhc_new(cybiko_t *m, bool verbose);
/* The emulator's thread: call after every emulated millisecond. */
void cy_usbhc_tick(cy_usbhc_t *h);
/* No more ticks: blocked transfers give up. */
void cy_usbhc_stop(cy_usbhc_t *h);
/* The client's thread. Until it parks, the emulator waits for it every
 * frame. */
void cy_usbhc_park(cy_usbhc_t *h, bool parked);
int cy_usbhc_enumerate(cy_usbhc_t *h, unsigned timeout_ms);
/* The device dropped off the bus since the last enumeration. */
bool cy_usbhc_detached(cy_usbhc_t *h);
cyusb_dev_t *cy_usbhc_dev(cy_usbhc_t *h);
void cy_usbhc_wait_ms(cy_usbhc_t *h, unsigned ms);
uint64_t cy_usbhc_ms(cy_usbhc_t *h);
