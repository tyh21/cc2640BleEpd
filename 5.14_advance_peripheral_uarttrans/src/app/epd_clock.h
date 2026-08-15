#ifndef EPD_CLOCK_H
#define EPD_CLOCK_H

#include <stdint.h>

// ---- Time structure ----
typedef struct {
    uint16_t year;        // e.g. 2026 (stored as 2000 + year_lo internally)
    uint8_t  month;       // 1-12
    uint8_t  day;         // 1-31
    uint8_t  hour;        // 0-23
    uint8_t  minute;      // 0-59
    uint8_t  second;      // 0-59
    uint16_t millisecond;  // 0-999
} epd_time_t;

// ---- API ----

// Initialize clock subsystem (call once at startup)
void epd_clock_init(void);

// Set time from raw BLE data (8 bytes: year_lo, month, day, hour, minute, second, ms_hi, ms_lo)
// year_lo: year - 2000 (e.g. 26 for 2026)
void epd_clock_set_time(const uint8_t *data);

// Get current time
epd_time_t epd_clock_get_time(void);

// Get current time as raw 8-byte array (for BLE response)
// out: year_lo, month, day, hour, minute, second, ms_hi, ms_lo
void epd_clock_get_time_raw(uint8_t *out);

// Render calendar display to EPD
// partial=0: full refresh (flicker, ~5s, on date change or first set_time)
// partial=1: partial refresh (no flicker, ~0.3-1s, on minute change)
// Layout: date line (y=0~15), HH:MM (y=20~51), lunar line (y=56~71)
void epd_clock_render(uint8_t partial);

// Called every second by Clock callback (posts event to EPD task)
void epd_clock_tick(void);

// Check if clock display needs refresh (minute boundary)
// Returns 0=no, 1=partial refresh, 2=force full refresh
uint8_t epd_clock_need_refresh(void);

// Switch clock face style (0=dot-matrix <-> 1=7-segment LED) and force full refresh
void epd_clock_switch_face(void);

// Get current clock face style (0=dot-matrix, 1=7-segment LED)
uint8_t epd_clock_get_face(void);

#endif // EPD_CLOCK_H
