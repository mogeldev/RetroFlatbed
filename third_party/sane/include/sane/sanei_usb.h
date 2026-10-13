/* Subset of SANE's sanei_usb API, implemented on top of WinUSB
 * (see src/winusb_usb.c). Only what the plustek/LM983x code needs. */
#ifndef LIDE20_SANEI_USB_H
#define LIDE20_SANEI_USB_H

#include "sane.h"

#ifdef __cplusplus
extern "C" {
#endif

void        sanei_usb_init(void);
void        sanei_usb_exit(void);
SANE_Status sanei_usb_find_devices(SANE_Int vendor, SANE_Int product,
                                   SANE_Status (*attach)(SANE_String_Const devname));
SANE_Status sanei_usb_open(SANE_String_Const devname, SANE_Int *dn);
void        sanei_usb_close(SANE_Int dn);
SANE_Status sanei_usb_get_vendor_product(SANE_Int dn, SANE_Word *vendor, SANE_Word *product);
SANE_Status sanei_usb_read_bulk(SANE_Int dn, SANE_Byte *buffer, size_t *size);
SANE_Status sanei_usb_write_bulk(SANE_Int dn, const SANE_Byte *buffer, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
