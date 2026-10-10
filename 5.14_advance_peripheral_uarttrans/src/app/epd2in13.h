
#ifndef EPD_2IN13_H
#define EPD_2IN13_H

#include <stdint.h>

// Display resolution — HINK-E0213A07 (SSD1680, 2.13" landscape)
// SSD1680 RAM: 250 scan lines × 16 bytes/row (128 px/row)
// Visible area: 212 lines × 13 bytes (104 pixels)
#define EPD_2IN13_WIDTH       212
#define EPD_2IN13_HEIGHT      104

// #define EPD_2IN13_FULL			0
// #define EPD_2IN13_PART			1

void epd_hw_init();

// ---- Panel arbitration ----
// The panel (SPI0 + the SSD1680 RAM) has three users: the clock render and the
// BLE image path (both run in the EPD task) and the NFC respondent (its own
// task).  Every user must hold this lock for the whole time it touches the
// panel or its RAM, so two producers can never interleave a command/data
// sequence (which would clobber the shared RAM address counter).  Acquire is
// blocking; Release must follow every Acquire exactly once.
void EPD_Panel_Acquire(void);
void EPD_Panel_Release(void);

// Low-level SPI commands (exposed for gfx library)
void EPD_2IN13_SendCommand(uint8_t Reg);
void EPD_2IN13_SendData(uint8_t Data);

void EPD_2IN13_Init();
void EPD_2IN13_InitPartial();   // partial refresh init (no sleep after)
void EPD_2IN13_SwitchToPartialLUT(void);  // switch LUT to partial (call after Init)
void EPD_2IN13_Clear(void);
void EPD_2IN13_Display(const uint8_t *Image);

void EPD_2IN13_PrepareBlkRAM(void);
void EPD_2IN13_PrepareRedRAM(void);
void EPD_2IN13_WriteRAM(const uint8_t *buf, const int len);

// Direct framebuffer write at an explicit byte offset (NFC streaming path).
//   pos = byte offset inside the 4000-byte SSD1680 window (0..3999)
//   X   = pos % 16      (one byte = 8 pixels)
//   Y   = pos / 16      (0..249, the gate line)
// The B/W RAM address counter is re-loaded on every call, so a retransmitted
// packet simply rewrites the same cells instead of shifting the image.
void EPD_2IN13_WriteRAMAt(uint32_t pos, const uint8_t *buf, int len);

// Non-blocking busy probe: 1 = panel busy, 0 = idle.  (EPD_2IN13_ReadBusy()
// waits; this one lets a real-time task poll instead.)
uint8_t EPD_2IN13_IsBusy(void);

// Number of BUSY waits that ran out of budget since boot (0 = healthy).
// A non-zero value means the panel did not answer - usually "panel not
// connected" or "left latched by a previous firmware".
uint16_t EPD_2IN13_BusyStalls(void);

// Kick off a full refresh and return AT ONCE, without waiting for BUSY.
// The NFC responder needs this: the reader is answered in a 9.7 ms window
// while a 2.13" update takes seconds.  Poll EPD_2IN13_IsBusy() afterwards.
void EPD_2IN13_UpdateDisplayAsync(void);

void EPD_2IN13_UpdateDisplay(void);    // full refresh
void EPD_2IN13_UpdatePartial(void);   // partial refresh (no flicker)
void EPD_2IN13_QuickCmd(uint8_t cmd);
void EPD_2IN13_LedTest(void);
void EPD_2IN13_LedToggle(void);

// Partial refresh: write image data with proper 0x24 + 0x26 handling
void EPD_2IN13_DisplayPartBaseImage(const uint8_t *Image);  // first time: 0x24+0x26 = same image
void EPD_2IN13_DisplayPart(const uint8_t *Image);           // subsequent: 0x24=new, 0x26=~new

void EPD_2IN13_Sleep(void);
void EPD_2IN13_PowerOff(void);   // power off without deep sleep (for partial mode)

// Power the analog block down but KEEP VCC + the RAM contents, so a plain
// "0x22 0xC7 / 0x20" update can resume the panel.  Deep sleep (plus cutting
// the PWR pin) is unrecoverable inside an ISO-DEP exchange, and that is what
// used to make the NFC image path land in a dead framebuffer - see the
// comment on the implementation.
void EPD_2IN13_SleepLight(void);

// Re-load VCOM + the 70-byte full-refresh LUT without a full bring-up.
// Used by the NFC image path to undo the clock's partial waveform.
void EPD_2IN13_LoadFullLUT(void);

#endif
