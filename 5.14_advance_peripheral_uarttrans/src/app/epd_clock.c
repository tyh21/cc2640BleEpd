/*
 * epd_clock.c - Calendar clock for BLE EPD tag
 * 
 * Features:
 * - Software time keeping via TI-RTOS Clock 1s tick (no 32kHz crystal needed)
 * - Full calendar display: date line, HH:MM (3x scaled to 24x48), lunar date
 * - 11x16 ASCII + 16x16 Chinese fonts (from epd_font.h)
 * - Lunar calendar via lunar.h
 * - Partial refresh (no flicker), full refresh on date change
 * - No static framebuffer (saves SRAM for BLE stack)
 * - Minimal stack usage: no large segment arrays on stack
 */

#include <string.h>
#include <stdint.h>
#include "epd_clock.h"
#include "epd2in13.h"
#include "epd_font.h"
#include "lunar.h"
#include "hw_uart.h"

// =====================================================================
// Civil date algorithms (Howard Hinnant's "date" library, public domain)
// =====================================================================

static int32_t days_from_civil(int32_t y, int32_t m, int32_t d) {
    y -= (m <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int32_t *y, int32_t *m, int32_t *d) {
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t dd = doy - (153 * mp + 2) / 5 + 1;
    uint32_t mm = mp + (mp < 10 ? 3 : -9);
    yy += (mm <= 2);
    *y = yy;
    *m = (int32_t)mm;
    *d = (int32_t)dd;
}

#define EPOCH_2000_OFFSET 10957

// =====================================================================
// State
// =====================================================================

static volatile uint32_t s_tick_seconds = 0;
static uint32_t s_set_epoch_sec = 0;
static uint32_t s_set_tick = 0;
static uint8_t  s_time_is_set = 0;
static uint8_t  s_force_refresh = 0;
static uint8_t  s_last_minute = 0xFF;
static uint16_t s_last_day_code = 0xFFFF;

// Clock face style: 0 = dot-matrix font (8x16 4x scaled + bilinear+Bayer)
//                   1 = 7-segment LED vector
static volatile uint8_t s_clock_face = 0;

// =====================================================================
// 7-segment LED vector drawing
// =====================================================================

// Segment encoding: b6~b0 = LU LD MU MM MD RU RD
//   b6=左上竖  b5=左下竖  b4=上横  b3=中横  b2=下横  b1=右上竖  b0=右下竖
static const uint8_t FONT_LED_CODE[11] = {
    0x77, // 0
    0x03, // 1
    0x3e, // 2
    0x1f, // 3
    0x4b, // 4
    0x5d, // 5
    0x7d, // 6
    0x13, // 7
    0x7f, // 8
    0x5f, // 9
    0x00  // blank (index 10)
};

// 7-segment parameters (tuned for 212x104 landscape)
#define LED_SEG_S       6    // segment line width (pixels)
#define LED_DIG_W       32   // digit width (reduced from 42)
#define LED_DIG_H       57   // digit height (reduced from 72, ~7px less top & bottom)
#define LED_DIG_MID     28   // midline (LED_DIG_H / 2, rounded)
#define LED_COLON_W     8    // colon width (reduced from 10)
#define LED_SPACING     3    // gap between elements (reduced from 4)

// Total width: 4*digit + colon + 4*spacing = 4*42 + 10 + 4*4 = 194
#define LED_TOTAL_W     (4*LED_DIG_W + LED_COLON_W + 4*LED_SPACING)

// =====================================================================
// Font: 8x16 pixels for HH:MM (4x scaled to 32x64, bilinear + Bayer dither)
// Bayer 4x4 threshold matrix (scaled to 0..255 range for 8-bit compare)
static const uint8_t bayer4x4[16] = {
     15, 135,  45, 165,
    195,  75, 225, 105,
     60, 180,  30, 150,
    240, 120, 210,  90
};

static const uint8_t font8x16[12][16] = {
    {0x00,0x00,0x3C,0x7E,0xC3,0xC3,0xC3,0xC3,
     0xC3,0xC3,0xC3,0xC3,0x7E,0x3C,0x00,0x00},
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,
     0x18,0x18,0x18,0x18,0x7E,0x7E,0x00,0x00},
    {0x00,0x00,0x7E,0xFF,0x03,0x03,0x07,0x0E,
     0x1C,0x38,0x70,0xE0,0xFF,0xFF,0x00,0x00},
    {0x00,0x00,0x7E,0xFF,0x03,0x03,0x07,0x1C,
     0x07,0x03,0x03,0xC3,0xFF,0x7E,0x00,0x00},
    {0x00,0x00,0x07,0x0F,0x1F,0x33,0x63,0xC3,
     0xFF,0xFF,0x03,0x03,0x03,0x03,0x00,0x00},
    {0x00,0x00,0xFF,0xFF,0xC0,0xC0,0xFE,0xFF,
     0x03,0x03,0x03,0xC3,0xFF,0x7E,0x00,0x00},
    {0x00,0x00,0x07,0x0F,0x1C,0x38,0x70,0xFE,
     0xFF,0xC3,0xC3,0xC3,0xFF,0x7E,0x00,0x00},
    {0x00,0x00,0xFF,0xFF,0x03,0x03,0x06,0x0C,
     0x0C,0x18,0x18,0x30,0x30,0x60,0x00,0x00},
    {0x00,0x00,0x7E,0xFF,0xC3,0xC3,0x7E,0x7E,
     0xC3,0xC3,0xC3,0xC3,0xFF,0x7E,0x00,0x00},
    {0x00,0x00,0x7E,0xFF,0xC3,0xC3,0xC3,0xFF,
     0x7F,0x03,0x07,0x0E,0x1C,0x38,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,
     0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
};

// =====================================================================
// Layout constants — Landscape 212x104
// Line 1 (y=0~15):  lunar line (16x16 CN)
// Line 2 (y=20~83): "HH:MM" (8x16 font, 4x scaled = 32x64)
// Line 3 (y=88~103): date line (11x16 ASCII + 16x16 CN)
// =====================================================================

#define CLOCK_Y_START    20
#define CLOCK_HEIGHT     64
#define CLOCK_NUM_CHARS  5
#define CLOCK_CHAR_W     32
#define CLOCK_SCALE      4

#define LUNAR_Y_START    0
#define LUNAR_HEIGHT     16

#define DATE_Y_START     88
#define DATE_HEIGHT      16

// =====================================================================
// UTF-8 helper: decode one UTF-8 char, return Unicode, advance index
// =====================================================================

static uint16_t utf8_decode(const char *str, int *idx)
{
    uint8_t c = (uint8_t)str[*idx];
    if (c == 0) return 0;
    if (c < 0x80) {
        (*idx)++;
        return (uint16_t)c;
    }
    if ((c & 0xE0) == 0xC0 && str[*idx + 1]) {
        uint16_t u = ((uint16_t)(c & 0x1F) << 6) | ((uint8_t)str[*idx + 1] & 0x3F);
        *idx += 2;
        return u;
    }
    if ((c & 0xF0) == 0xE0 && str[*idx + 1] && str[*idx + 2]) {
        uint16_t u = ((uint16_t)(c & 0x0F) << 12)
                   | ((uint16_t)((uint8_t)str[*idx + 1] & 0x3F) << 6)
                   | ((uint16_t)((uint8_t)str[*idx + 2] & 0x3F));
        *idx += 3;
        return u;
    }
    (*idx)++;
    return (uint16_t)c;
}

// =====================================================================
// Pixel getter for font bitmaps
// Returns 1=black, 0=white
// For ASCII 11x16: bitmap is 32 bytes (2 bytes/row, MSB left), dx 0..10
// For CN 16x16: bitmap is 32 bytes (2 bytes/row, MSB left), dx 0..15
// =====================================================================

static uint8_t font_pixel(const uint8_t *bitmap, int dx, int font_row, int is_cn)
{
    int w = is_cn ? CN_FONT_W : ASCII_FONT_W;  // 16 or 11
    if (dx < 0 || dx >= w || font_row < 0 || font_row >= 16) return 0;
    uint8_t byte_idx = dx / 8;
    uint8_t bit_idx = 7 - (dx % 8);
    return (bitmap[font_row * 2 + byte_idx] >> bit_idx) & 1;
}

// =====================================================================
// Render a mixed ASCII+Chinese text line at given y offset
// Walks the UTF-8 string for each x column to find which char covers it.
// This avoids allocating segment arrays on the stack.
// =====================================================================

static void render_text_line(int x, uint8_t *row_buf, const char *str,
                              int line_x_start, int y_start, int y_height)
{
    // Find which character covers column x
    int cur_x = line_x_start;
    int si = 0;
    
    while (str[si]) {
        uint8_t c = (uint8_t)str[si];
        
        if (c == ' ') {
            // Space: ASCII width gap
            if (x >= cur_x && x < cur_x + ASCII_FONT_W) {
                // space = blank, do nothing (already white)
                return;  // no pixel to set
            }
            cur_x += ASCII_FONT_W;
            si++;
            continue;
        }
        
        if (c < 0x80) {
            // ASCII char
            if (x >= cur_x && x < cur_x + ASCII_FONT_W) {
                const uint8_t *bmp = epd_font_get_ascii_bitmap((char)c);
                if (bmp) {
                    int dx = x - cur_x;
                    int y;
                    for (y = y_start; y < y_start + y_height && y < 104; y++) {
                        int font_row = 15 - (y - y_start);
                        uint8_t pixel = font_pixel(bmp, dx, font_row, 0);
                        if (pixel) {
                            int byte_idx = y >> 3;
                            int bit_idx = 7 - (y & 7);
                            row_buf[byte_idx] &= ~(1 << bit_idx);
                        }
                    }
                }
                return;
            }
            cur_x += ASCII_FONT_W;
            si++;
        } else {
            // UTF-8 Chinese char (3 bytes)
            int tmp_i = si;
            uint16_t unicode = utf8_decode(str, &tmp_i);
            const uint8_t *bmp = epd_font_get_cn_bitmap(unicode);
            
            if (x >= cur_x && x < cur_x + CN_FONT_W) {
                if (bmp) {
                    int dx = x - cur_x;
                    int y;
                    for (y = y_start; y < y_start + y_height && y < 104; y++) {
                        int font_row = 15 - (y - y_start);
                        uint8_t pixel = font_pixel(bmp, dx, font_row, 1);
                        if (pixel) {
                            int byte_idx = y >> 3;
                            int bit_idx = 7 - (y & 7);
                            row_buf[byte_idx] &= ~(1 << bit_idx);
                        }
                    }
                }
                return;
            }
            cur_x += CN_FONT_W;
            si = tmp_i;
        }
    }
}

// =====================================================================
// Calculate total pixel width of a text string
// =====================================================================

static int text_line_width(const char *str)
{
    int w = 0;
    int si = 0;
    while (str[si]) {
        uint8_t c = (uint8_t)str[si];
        if (c == ' ' || c < 0x80) {
            w += ASCII_FONT_W;
            si++;
        } else {
            w += CN_FONT_W;
            si += 3;  // UTF-8 3-byte
        }
    }
    return w;
}

// =====================================================================
// Weekday calculation
// =====================================================================

static const char *weekday_name(uint16_t year, uint8_t month, uint8_t day)
{
    int32_t y = year, m = month, d = day;
    if (m < 3) { m += 12; y--; }
    int32_t w = (d + 2*m + 3*(m+1)/5 + y + y/4 - y/100 + y/400) % 7;
    return WEEKCN[w + 1];
}

// =====================================================================
// API implementation
// =====================================================================

void epd_clock_tick(void) {
    s_tick_seconds++;
}

void epd_clock_init(void) {
    s_time_is_set = 0;
    s_force_refresh = 0;
    s_last_minute = 0xFF;
    s_last_day_code = 0xFFFF;
    s_clock_face = 0;
}

// Switch clock face style and force full refresh
void epd_clock_switch_face(void) {
    s_clock_face = (s_clock_face == 0) ? 1 : 0;
    s_force_refresh = 1;
    s_last_minute = 0xFF;       // force re-render
    s_last_day_code = 0xFFFF;
}

// Get current clock face style (0=dot-matrix, 1=7-segment LED)
uint8_t epd_clock_get_face(void) {
    return s_clock_face;
}

void epd_clock_set_time(const uint8_t *data) {
    uint16_t year = 2000U + data[0];
    uint8_t  month = data[1];
    uint8_t  day = data[2];
    uint8_t  hour = data[3];
    uint8_t  minute = data[4];
    uint8_t  second = data[5];
    uint16_t ms = ((uint16_t)data[6] << 8) | data[7];

    int32_t days = days_from_civil((int32_t)year, (int32_t)month, (int32_t)day) - EPOCH_2000_OFFSET;
    s_set_epoch_sec = (uint32_t)days * 86400U + (uint32_t)hour * 3600U
                      + (uint32_t)minute * 60U + (uint32_t)second;
    s_set_tick = s_tick_seconds;
    s_time_is_set = 1;
    s_force_refresh = 1;
    s_last_minute = 0xFF;
    s_last_day_code = 0xFFFF;

    HWUART_Printf("set time: %d-%d-%d %d:%d:%d.%d\r\n", year, month, day, hour, minute, second, ms);
}

epd_time_t epd_clock_get_time(void) {
    epd_time_t t;
    memset(&t, 0, sizeof(t));

    if (!s_time_is_set) return t;

    uint32_t elapsed = s_tick_seconds - s_set_tick;
    uint32_t current = s_set_epoch_sec + elapsed;

    int32_t days = (int32_t)(current / 86400U);
    uint32_t day_sec = current % 86400U;

    int32_t y, m, d;
    civil_from_days(days + EPOCH_2000_OFFSET, &y, &m, &d);

    t.year = (uint16_t)y;
    t.month = (uint8_t)m;
    t.day = (uint8_t)d;
    t.hour = (uint8_t)(day_sec / 3600U);
    t.minute = (uint8_t)((day_sec % 3600U) / 60U);
    t.second = (uint8_t)(day_sec % 60U);
    t.millisecond = 0;

    return t;
}

void epd_clock_get_time_raw(uint8_t *out) {
    epd_time_t t = epd_clock_get_time();
    out[0] = (uint8_t)(t.year - 2000U);
    out[1] = t.month;
    out[2] = t.day;
    out[3] = t.hour;
    out[4] = t.minute;
    out[5] = t.second;
    out[6] = (uint8_t)(t.millisecond >> 8);
    out[7] = (uint8_t)(t.millisecond & 0xFF);
}

uint8_t epd_clock_need_refresh(void) {
    if (!s_time_is_set) return 0;

    if (s_force_refresh) {
        s_force_refresh = 0;
        return 2;
    }

    epd_time_t t = epd_clock_get_time();
    
    uint16_t day_code = (uint16_t)t.year * 512 + (uint16_t)t.month * 32 + t.day;
    if (day_code != s_last_day_code) {
        s_last_day_code = day_code;
        s_last_minute = t.minute;
        return 2;
    }
    
    if (t.minute != s_last_minute) {
        s_last_minute = t.minute;
        return 1;
    }
    return 0;
}

// =====================================================================
// 7-segment LED: render one column of a digit into row_buf
// x_offset: column index within the digit (0..LED_DIG_W-1)
// digit: 0..9
// y_start: top of digit area on screen
// =====================================================================

static void led_digit_render_col(int x_offset, uint8_t digit, int y_start,
                                  uint8_t *row_buf)
{
    if (digit > 10) digit = 10;
    uint8_t code = FONT_LED_CODE[digit];
    int y;

    for (y = 0; y < LED_DIG_H && (y_start + y) < 104; y++) {
        int screen_y = y_start + y;
        int pixel = 0;

        // Y direction matches text rendering: y=0 → digit bottom, y=LED_DIG_H-1 → digit top
        // (same logic as font_row = 15 - (y - y_start) for text)

        // Horizontal segments (bottom, mid, top — from y=0 upward)
        if (x_offset >= LED_SEG_S && x_offset < LED_DIG_W - LED_SEG_S) {
            if (y < LED_SEG_S)                     pixel = (code >> 2) & 1; // MD: bottom (at y=0)
            else if (y >= LED_DIG_H - LED_SEG_S)    pixel = (code >> 4) & 1; // MU: top (at y=H-1)
            else if (y >= LED_DIG_MID - LED_SEG_S/2 && y < LED_DIG_MID + LED_SEG_S/2)
                                                    pixel = (code >> 3) & 1; // MM: middle
        }

        // Left vertical segments (Y-flipped: y<MID = lower half of digit, y>=MID = upper half)
        if (x_offset < LED_SEG_S) {
            if (y >= LED_SEG_S && y < LED_DIG_MID)  pixel = (code >> 5) & 1; // LD (lower)
            else if (y >= LED_DIG_MID && y < LED_DIG_H - LED_SEG_S)
                                                    pixel = (code >> 6) & 1; // LU (upper)
        }

        // Right vertical segments (Y-flipped: y<MID = lower half, y>=MID = upper half)
        if (x_offset >= LED_DIG_W - LED_SEG_S) {
            if (y >= LED_SEG_S && y < LED_DIG_MID)  pixel = (code >> 0) & 1; // RD (lower)
            else if (y >= LED_DIG_MID && y < LED_DIG_H - LED_SEG_S)
                                                    pixel = (code >> 1) & 1; // RU (upper)
        }

        if (pixel) {
            int byte_idx = screen_y >> 3;
            int bit_idx = 7 - (screen_y & 7);
            row_buf[byte_idx] &= ~(1 << bit_idx);
        }
    }
}

// =====================================================================
// 7-segment LED: render one column of the colon
// x_offset: column index within colon area (0..LED_COLON_W-1)
// y_start: top of digit area on screen (same as digit y_start)
// =====================================================================

static void led_colon_render_col(int x_offset, int y_start, uint8_t *row_buf)
{
    // Two squares: at 1/3 and 2/3 height, each LED_SEG_S x LED_SEG_S
    int dot_x_start = (LED_COLON_W - LED_SEG_S) / 2;
    int dot_x_end = dot_x_start + LED_SEG_S;
    if (x_offset < dot_x_start || x_offset >= dot_x_end) return;

    int y;
    for (y = 0; y < LED_DIG_H && (y_start + y) < 104; y++) {
        int screen_y = y_start + y;
        int pixel = 0;

        // Top dot: at 1/3 height
        int top_center = LED_DIG_H / 3;
        if (y >= top_center - LED_SEG_S/2 && y < top_center + LED_SEG_S/2)
            pixel = 1;

        // Bottom dot: at 2/3 height
        int bot_center = LED_DIG_H * 2 / 3;
        if (y >= bot_center - LED_SEG_S/2 && y < bot_center + LED_SEG_S/2)
            pixel = 1;

        if (pixel) {
            int byte_idx = screen_y >> 3;
            int bit_idx = 7 - (screen_y & 7);
            row_buf[byte_idx] &= ~(1 << bit_idx);
        }
    }
}

// =====================================================================
// Render one column (x) of the full 212x104 image
// row_buf[16] covers y=0..103 (bit 7=y0 of each byte)
// Pixel: 0xFF=white, 0=black
// =====================================================================

static void render_row(int x, uint8_t *row_buf,
                       const char *date_str, int date_x,
                       const uint8_t *clock_chars,
                       const char *lunar_str, int lunar_x)
{
    memset(row_buf, 0xFF, 16);  // white background

    int y;
    
    // ---- Lunar line (y=0~15, now top) ----
    render_text_line(x, row_buf, lunar_str, lunar_x, LUNAR_Y_START, LUNAR_HEIGHT);
    
    // ---- HH:MM line ----
    if (s_clock_face == 0) {
        // ---- Face 0: dot-matrix font, 4x scaled to 32x64, bilinear+Bayer ----
        int clock_x_start = (212 - CLOCK_NUM_CHARS * CLOCK_CHAR_W) / 2;
        if (x >= clock_x_start && x < clock_x_start + CLOCK_NUM_CHARS * CLOCK_CHAR_W) {
            int char_idx = (x - clock_x_start) / CLOCK_CHAR_W;
            int char_x   = (x - clock_x_start) % CLOCK_CHAR_W;
            const uint8_t *bmp = font8x16[clock_chars[char_idx]];
            
            for (y = CLOCK_Y_START; y < CLOCK_Y_START + CLOCK_HEIGHT && y < 104; y++) {
                int sy = y - CLOCK_Y_START;
                
                // Bilinear interpolation in fixed-point (8 frac bits)
                // 8x16 font: fx ranges 0..7, fy ranges 0..15 (inverted)
                int fx = (char_x << 8) * 8 / CLOCK_CHAR_W;
                int fy = ((CLOCK_HEIGHT - 1 - sy) << 8) * 16 / CLOCK_HEIGHT;
                
                int fx0 = fx >> 8;  int fx1 = (fx0 < 7) ? fx0 + 1 : fx0;
                int fy0 = fy >> 8;  int fy1 = (fy0 < 15) ? fy0 + 1 : fy0;
                int dx = fx & 0xFF;
                int dy = fy & 0xFF;
                
                // Read 4 source pixels (1 byte/row, MSB left)
                int p00 = ((bmp[fy0] >> (7 - fx0)) & 1) ? 255 : 0;
                int p10 = ((bmp[fy0] >> (7 - fx1)) & 1) ? 255 : 0;
                int p01 = ((bmp[fy1] >> (7 - fx0)) & 1) ? 255 : 0;
                int p11 = ((bmp[fy1] >> (7 - fx1)) & 1) ? 255 : 0;
                
                // Bilinear blend
                int val = (p00 * (256 - dx) + p10 * dx) * (256 - dy)
                        + (p01 * (256 - dx) + p11 * dx) * dy;
                uint8_t gray = (uint8_t)(val >> 16);
                
                // Bayer 4x4 ordered dither
                uint8_t threshold = bayer4x4[((sy & 3) << 2) | (char_x & 3)];
                uint8_t pixel = (gray > threshold) ? 1 : 0;
                
                if (pixel) {
                    int byte_idx = y >> 3;
                    int bit_idx = 7 - (y & 7);
                    row_buf[byte_idx] &= ~(1 << bit_idx);
                }
            }
        }
    } else {
        // ---- Face 1: 7-segment LED vector ----
        // Layout: H1 [sp] H2 [sp] : [sp] M1 [sp] M2
        // LED digit y = centered in 104: (104 - LED_DIG_H) / 2 = 16
        // But we keep lunar at y=0~15 and date at y=88~103,
        // so LED digits go in the middle: y = LUNAR_HEIGHT + (104 - LUNAR_HEIGHT - DATE_HEIGHT - LED_DIG_H) / 2
        // = 16 + (104 - 16 - 16 - 72) / 2 = 16 + 0 = 16
        // Actually let's center the LED in the gap y=16~87 (72px gap, 72px digit = perfect)
        // LED digits centered in gap y=16~87 (72px gap, LED_DIG_H=57)
        int led_y = LUNAR_Y_START + LUNAR_HEIGHT + (88 - LUNAR_Y_START - LUNAR_HEIGHT - LED_DIG_H) / 2;
        int led_x_start = (212 - LED_TOTAL_W) / 2;
        
        if (x >= led_x_start && x < led_x_start + LED_TOTAL_W) {
            int rel_x = x - led_x_start;
        // Layout: H1 H2 : M1 M2 (no X-mirror, natural order)
        int pos1_start = LED_DIG_W + LED_SPACING;
        int colon_start = pos1_start + LED_DIG_W + LED_SPACING;
        int pos2_start = colon_start + LED_COLON_W + LED_SPACING;
        int pos3_start = pos2_start + LED_DIG_W + LED_SPACING;

        if (x < led_x_start + LED_DIG_W) {
            led_digit_render_col(rel_x, clock_chars[0], led_y, row_buf);
        } else if (x >= led_x_start + pos1_start && x < led_x_start + pos1_start + LED_DIG_W) {
            led_digit_render_col(rel_x - pos1_start, clock_chars[1], led_y, row_buf);
        } else if (x >= led_x_start + colon_start && x < led_x_start + colon_start + LED_COLON_W) {
            led_colon_render_col(rel_x - colon_start, led_y, row_buf);
        } else if (x >= led_x_start + pos2_start && x < led_x_start + pos2_start + LED_DIG_W) {
            led_digit_render_col(rel_x - pos2_start, clock_chars[3], led_y, row_buf);
        } else if (x >= led_x_start + pos3_start && x < led_x_start + pos3_start + LED_DIG_W) {
            led_digit_render_col(rel_x - pos3_start, clock_chars[4], led_y, row_buf);
        }
        }
    }
    
    // ---- Date line (y=88~103, now bottom) ----
    render_text_line(x, row_buf, date_str, date_x, DATE_Y_START, DATE_HEIGHT);
}

// =====================================================================
// Helper: append UTF-8 Chinese char to buffer
// =====================================================================

static int append_utf8_cn(char *buf, int pos, const char *cn_str)
{
    int k = 0;
    while (cn_str[k] && pos < 62) { buf[pos++] = cn_str[k++]; }
    return pos;
}

// =====================================================================
// Main render function
// =====================================================================

void epd_clock_render(uint8_t partial) {
    if (!s_time_is_set) return;

    epd_time_t t = epd_clock_get_time();
    s_last_minute = t.minute;
    s_last_day_code = (uint16_t)t.year * 512 + (uint16_t)t.month * 32 + t.day;

    HWUART_Printf("render: %d-%d-%d %d:%d (%s)\r\n", 
                   t.year, t.month, t.day, t.hour, t.minute, partial ? "P" : "F");

    // ---- Build date string: "2026年08月12日 星期三" ----
    // Use hex escape sequences for Chinese chars (IAR compatibility)
    char date_buf[64];
    {
        int p = 0;
        date_buf[p++] = '0' + (t.year / 1000) % 10;
        date_buf[p++] = '0' + (t.year / 100) % 10;
        date_buf[p++] = '0' + (t.year / 10) % 10;
        date_buf[p++] = '0' + t.year % 10;
        p = append_utf8_cn(date_buf, p, "\xE5\xB9\xB4");  /* 年 */
        date_buf[p++] = '0' + t.month / 10;
        date_buf[p++] = '0' + t.month % 10;
        p = append_utf8_cn(date_buf, p, "\xE6\x9C\x88");  /* 月 */
        date_buf[p++] = '0' + t.day / 10;
        date_buf[p++] = '0' + t.day % 10;
        p = append_utf8_cn(date_buf, p, "\xE6\x97\xA5");  /* 日 */
        date_buf[p++] = ' ';
        p = append_utf8_cn(date_buf, p, "\xE6\x98\x9F");  /* 星 */
        p = append_utf8_cn(date_buf, p, "\xE6\x9C\x9F");  /* 期 */
        {
            const char *wd = weekday_name(t.year, t.month, t.day);
            p = append_utf8_cn(date_buf, p, wd);
        }
        date_buf[p] = '\0';
    }

    // ---- Build lunar string: "丙午年 七月廿九" ----
    char lunar_buf[48];
    {
        struct Lunar_Date lunar;
        LUNAR_SolarToLunar(&lunar, t.year, t.month, t.day);
        
        int p = 0;
        p = append_utf8_cn(lunar_buf, p, Lunar_StemStrig[LUNAR_GetStem(&lunar)]);
        p = append_utf8_cn(lunar_buf, p, Lunar_BranchStrig[LUNAR_GetBranch(&lunar)]);
        p = append_utf8_cn(lunar_buf, p, "\xE5\xB9\xB4");  /* 年 */
        lunar_buf[p++] = ' ';  // space
        if (lunar.IsLeap) {
            p = append_utf8_cn(lunar_buf, p, Lunar_MonthLeapString[1]);  /* 闰 */
        }
        p = append_utf8_cn(lunar_buf, p, Lunar_MonthString[lunar.Month]);
        p = append_utf8_cn(lunar_buf, p, Lunar_DateString[lunar.Date]);
        lunar_buf[p] = '\0';
    }

    // ---- Calculate x positions (center each line) ----
    int date_x = (212 - text_line_width(date_buf)) / 2;
    int lunar_x = (212 - text_line_width(lunar_buf)) / 2;

    // ---- HH:MM char indices ----
    uint8_t clock_chars[5] = {
        (uint8_t)(t.hour / 10),
        (uint8_t)(t.hour % 10),
        10,  // ':'
        (uint8_t)(t.minute / 10),
        (uint8_t)(t.minute % 10)
    };

    // ---- Render to EPD ----
    uint8_t row_buf[16];
    int x, col;

    if (partial) {
        EPD_2IN13_Init();
        EPD_2IN13_SwitchToPartialLUT();

        // Write new image to 0x24
        EPD_2IN13_SendCommand(0x4E);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x4F);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x24);
        for (x = 0; x < 250; x++) {
            if (x < 212) {
                render_row(x, row_buf, date_buf, date_x, clock_chars, lunar_buf, lunar_x);
                EPD_2IN13_WriteRAM(row_buf, 16);
            } else {
                for (col = 0; col < 16; col++) EPD_2IN13_SendData(0xFF);
            }
        }

        // Write inverted new image to 0x26
        EPD_2IN13_SendCommand(0x4E);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x4F);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x26);
        for (x = 0; x < 250; x++) {
            if (x < 212) {
                render_row(x, row_buf, date_buf, date_x, clock_chars, lunar_buf, lunar_x);
                for (col = 0; col < 16; col++) EPD_2IN13_SendData(~row_buf[col]);
            } else {
                for (col = 0; col < 16; col++) EPD_2IN13_SendData(0x00);
            }
        }

        EPD_2IN13_UpdatePartial();
        EPD_2IN13_Sleep();
    } else {
        EPD_2IN13_Init();

        // Write new image to 0x24
        EPD_2IN13_SendCommand(0x4E);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x4F);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x24);
        for (x = 0; x < 250; x++) {
            if (x < 212) {
                render_row(x, row_buf, date_buf, date_x, clock_chars, lunar_buf, lunar_x);
                EPD_2IN13_WriteRAM(row_buf, 16);
            } else {
                for (col = 0; col < 16; col++) EPD_2IN13_SendData(0xFF);
            }
        }

        // Write SAME image to 0x26 (base image)
        EPD_2IN13_SendCommand(0x4E);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x4F);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x26);
        for (x = 0; x < 250; x++) {
            if (x < 212) {
                render_row(x, row_buf, date_buf, date_x, clock_chars, lunar_buf, lunar_x);
                EPD_2IN13_WriteRAM(row_buf, 16);
            } else {
                for (col = 0; col < 16; col++) EPD_2IN13_SendData(0xFF);
            }
        }

        EPD_2IN13_UpdateDisplay();
        EPD_2IN13_Sleep();
    }

    HWUART_Printf("calendar rendered\r\n");
}
