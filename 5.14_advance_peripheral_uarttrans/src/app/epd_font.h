/*
 * epd_font.h - Font data for EPD calendar display
 * 
 * Two font sets:
 * 1. EPD_ASCII_11X16: 11x16 pixel ASCII, ' ' to '~' (95 chars), 32 bytes each
 * 2. EPD_FontUTF8_16x16: 16x16 pixel Chinese (GB2312 subset, 66 chars)
 *    Format: 3-byte Unicode index + 32 bytes bitmap per char
 */

#ifndef EPD_FONT_H
#define EPD_FONT_H

#include <stdint.h>

// ASCII font: 11 wide x 16 tall, 95 chars (' ' ~ '~')
// Layout: header [first_char, width, height, count] then 32 bytes per char
// Each char: 2 bytes per row × 16 rows = 32 bytes (MSB left)
extern const uint8_t EPD_ASCII_11X16[];

// Chinese font: 16 wide x 16 tall, 66 chars
// Layout: header [first_char_hi, first_char_lo, width, height, count] then per char:
//   3 bytes Unicode index + 32 bytes bitmap (2 bytes/row × 16 rows)
extern const uint8_t EPD_FontUTF8_16x16[];

// Helper: find Chinese char bitmap by Unicode code point
// Returns pointer to 32-byte bitmap, or NULL if not found
const uint8_t *epd_font_get_cn_bitmap(uint16_t unicode);

// Helper: find ASCII char bitmap by char value
// Returns pointer to 32-byte bitmap, or NULL if not found
const uint8_t *epd_font_get_ascii_bitmap(char ch);

// ASCII font dimensions
#define ASCII_FONT_W     11
#define ASCII_FONT_H     16
#define ASCII_FONT_BYTES 32  // 2 bytes per row × 16 rows

// Chinese font dimensions
#define CN_FONT_W     16
#define CN_FONT_H     16
#define CN_FONT_BYTES 32  // 2 bytes per row × 16 rows

#endif /* EPD_FONT_H */
