
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

#endif
