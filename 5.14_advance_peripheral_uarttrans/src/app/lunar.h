/*
 * lunar.h - Lunar calendar conversion (ported from DA14585 project)
 * 
 * Converts Gregorian date to Chinese lunar date (2000-2199).
 * Provides: lunar month, date, year, leap month, zodiac, stems/branches.
 */

#ifndef LUNAR_H
#define LUNAR_H

#include <stdint.h>

struct Lunar_Date {
    uint8_t  IsLeap;   // 1 if leap month
    uint8_t  Date;     // 1-30
    uint8_t  Month;    // 1-12
    uint16_t Year;     // lunar year
};

// Convert Gregorian to Lunar
void LUNAR_SolarToLunar(struct Lunar_Date *lunar,
                         uint16_t solar_year, uint8_t solar_month, uint8_t solar_date);

// Zodiac/stem/branch index getters
uint8_t LUNAR_GetZodiac(const struct Lunar_Date *lunar);   // 0-11
uint8_t LUNAR_GetStem(const struct Lunar_Date *lunar);     // 0-9
uint8_t LUNAR_GetBranch(const struct Lunar_Date *lunar);   // 0-11

// String tables (const, in Flash) - UTF-8 encoded hex escapes for IAR compatibility
// Each Chinese char = 3 bytes UTF-8 + null terminator
extern const char Lunar_MonthString[13][7];    // e.g. "正月" = 6 bytes + \0 = 7
extern const char Lunar_MonthLeapString[2][4];  // e.g. "闰" = 3 bytes + \0 = 4
extern const char Lunar_DateString[31][7];      // e.g. "初一" = 6 bytes + \0 = 7
extern const char Lunar_ZodiacString[12][4];    // single Chinese char = 3 + \0 = 4
extern const char Lunar_StemStrig[10][4];       // single Chinese char
extern const char Lunar_BranchStrig[12][4];     // single Chinese char
extern const char WEEKCN[8][4];                 // single Chinese char

#endif /* LUNAR_H */
