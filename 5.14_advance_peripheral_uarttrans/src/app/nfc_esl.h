/******************************************************************************

 @file  nfc_esl.h

 @brief FM11NC08S NFC channel + ISO14443-4 PICC responder for the ESL tag.

   The board carries an FM11NC08S, which is an NFC *channel* chip: FM11NC
   = channel, FM11NT = tag.  It does not answer anything by itself - it
   hands the MCU the ISO14443-4 frames and puts the MCU's reply on the
   air.  So the CC2640 IS the PICC here, and the whole ESL command set
   (VERIFY PIN, SET PARAM, LOAD IMAGE, REDRAW, GET BUSY) lives in this
   file.

   The 4000-byte LOAD IMAGE payload (F0 D2, 16 x 250 bytes) is streamed
   byte for byte into the SSD1680 framebuffer:

       phone  F0 D2 payload    250 B x 16 packets = 4000 bytes
       panel  SSD1680 RAM      16 B x 250 lines   = 4000 bytes

   Same size, same order - the ESL image IS the framebuffer, so no image
   buffer is ever allocated (which is what makes this fit next to the BLE
   stack).

   The chip appends CRC_A itself, so this code writes PCB + INF only.
   Everything here was validated on hardware against a real Android
   phone; a tap paints the panel.

   APDUs answered (CLA = F0):
     F0 D2 P1 P2 FA <250B>   LOAD IMAGE, P2 = packet index (0..15)
     F0 D4 05 80 00          REDRAW, returns at once, refresh runs on
     F0 DE 00 00 01          GET BUSY (00 idle / 01 busy)
     F0 D5 00|01 <mode> 01   DISPLAY MODE: P1=00 set, 01 read;
                             P2/the mode byte is 0 = clock, 1 = picture.
                             Answer = one data byte with the mode in effect.

   A finished redraw switches the panel to image mode, so the picture stays
   on screen instead of being repainted by the 1-minute clock tick.

 *****************************************************************************/

#ifndef NFC_ESL_H
#define NFC_ESL_H

#include <stdint.h>

// Create the NFC responder task.  Call once from main() next to
// TaskEPD_createTask().  The panel driver is initialised lazily on first
// use, so the order of the two task creations does not matter.
void NfcEsl_createTask(void);

// Non-zero while an ISO-DEP exchange is open or an image is streaming.
// Informational: the EPD task uses its own transfer flag to hold the
// clock off while NFC transfers are in progress.
uint8_t NfcEsl_busy(void);

// Hand the panel back to the clock: clears the clock-hold flags this
// module owns.  Called whenever the user explicitly selects clock mode
// (BLE 0x70 mode=0 / NFC F0 D5 P1=00 P2=00), so a hold left behind by an
// aborted tap cannot freeze the calendar off the screen for good.
void NfcEsl_yieldPanel(void);

#endif /* NFC_ESL_H */
