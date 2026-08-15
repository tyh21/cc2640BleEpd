/*
 * lunar.c - Lunar calendar conversion (ported from DA14585 project)
 * 
 * All const data goes to Flash, no SRAM usage.
 * Removed: DEV_Config.h dependency, 24-solar-term functions.
 * Chinese strings encoded as hex escape sequences for IAR compatibility.
 */

#include "lunar.h"
#include <string.h>

// Chinese strings as hex escape sequences (UTF-8)
// Each Chinese char = 3 bytes UTF-8, plus null terminator

const char Lunar_MonthString[13][7] = {
    "\xE6\x9C\xAA\xE7\x9F\xA5",         /* 未知 */
    "\xE6\xAD\xA3\xE6\x9C\x88",         /* 正月 */
    "\xE4\xBA\x8C\xE6\x9C\x88",         /* 二月 */
    "\xE4\xB8\x89\xE6\x9C\x88",         /* 三月 */
    "\xE5\x9B\x9B\xE6\x9C\x88",         /* 四月 */
    "\xE4\xBA\x94\xE6\x9C\x88",         /* 五月 */
    "\xE5\x85\xAD\xE6\x9C\x88",         /* 六月 */
    "\xE4\xB8\x83\xE6\x9C\x88",         /* 七月 */
    "\xE5\x85\xAB\xE6\x9C\x88",         /* 八月 */
    "\xE4\xB9\x9D\xE6\x9C\x88",         /* 九月 */
    "\xE5\x8D\x81\xE6\x9C\x88",         /* 十月 */
    "\xE5\x86\xAC\xE6\x9C\x88",         /* 冬月 */
    "\xE8\x85\x8A\xE6\x9C\x88"          /* 腊月 */
};

const char Lunar_MonthLeapString[2][4] = {
    "",
    "\xE9\x97\xB0"                       /* 闰 */
};

const char Lunar_DateString[31][7] = {
    "\xE6\x9C\xAA\xE7\x9F\xA5",         /* 未知 */
    "\xE5\x88\x9D\xE4\xB8\x80",         /* 初一 */
    "\xE5\x88\x9D\xE4\xBA\x8C",         /* 初二 */
    "\xE5\x88\x9D\xE4\xB8\x89",         /* 初三 */
    "\xE5\x88\x9D\xE5\x9B\x9B",         /* 初四 */
    "\xE5\x88\x9D\xE4\xBA\x94",         /* 初五 */
    "\xE5\x88\x9D\xE5\x85\xAD",         /* 初六 */
    "\xE5\x88\x9D\xE4\xB8\x83",         /* 初七 */
    "\xE5\x88\x9D\xE5\x85\xAB",         /* 初八 */
    "\xE5\x88\x9D\xE4\xB9\x9D",         /* 初九 */
    "\xE5\x88\x9D\xE5\x8D\x81",         /* 初十 */
    "\xE5\x8D\x81\xE4\xB8\x80",         /* 十一 */
    "\xE5\x8D\x81\xE4\xBA\x8C",         /* 十二 */
    "\xE5\x8D\x81\xE4\xB8\x89",         /* 十三 */
    "\xE5\x8D\x81\xE5\x9B\x9B",         /* 十四 */
    "\xE5\x8D\x81\xE4\xBA\x94",         /* 十五 */
    "\xE5\x8D\x81\xE5\x85\xAD",         /* 十六 */
    "\xE5\x8D\x81\xE4\xB8\x83",         /* 十七 */
    "\xE5\x8D\x81\xE5\x85\xAB",         /* 十八 */
    "\xE5\x8D\x81\xE4\xB9\x9D",         /* 十九 */
    "\xE4\xBA\x8C\xE5\x8D\x81",         /* 二十 */
    "\xE5\x8D\x84\xE4\xB8\x80",         /* 廿一 */
    "\xE5\x8D\x84\xE4\xBA\x8C",         /* 廿二 */
    "\xE5\x8D\x84\xE4\xB8\x89",         /* 廿三 */
    "\xE5\x8D\x84\xE5\x9B\x9B",         /* 廿四 */
    "\xE5\x8D\x84\xE4\xBA\x94",         /* 廿五 */
    "\xE5\x8D\x84\xE5\x85\xAD",         /* 廿六 */
    "\xE5\x8D\x84\xE4\xB8\x83",         /* 廿七 */
    "\xE5\x8D\x84\xE5\x85\xAB",         /* 廿八 */
    "\xE5\x8D\x84\xE4\xB9\x9D",         /* 廿九 */
    "\xE4\xB8\x89\xE5\x8D\x81"          /* 三十 */
};

const char Lunar_ZodiacString[12][4] = {
    "\xE7\x8C\xB4",                       /* 猴 */
    "\xE9\xB8\xA1",                       /* 鸡 */
    "\xE7\x8B\x97",                       /* 狗 */
    "\xE7\x8C\xAA",                       /* 猪 */
    "\xE9\xBC\xA0",                       /* 鼠 */
    "\xE7\x89\x9B",                       /* 牛 */
    "\xE8\x99\x8E",                       /* 虎 */
    "\xE5\x85\x94",                       /* 兔 */
    "\xE9\xBE\x99",                       /* 龙 */
    "\xE8\x9B\x87",                       /* 蛇 */
    "\xE9\xa9\xAC",                       /* 马 */
    "\xE7\xBE\x8A"                        /* 羊 */
};

const char Lunar_StemStrig[10][4] = {
    "\xE5\xBA\x9A",                       /* 庚 */
    "\xE8\xBE\x9B",                       /* 辛 */
    "\xE5\xA3\xAC",                       /* 壬 */
    "\xE7\x99\xB8",                       /* 癸 */
    "\xE7\x94\xB2",                       /* 甲 */
    "\xE4\xb9\x99",                       /* 乙 */
    "\xE4\xb8\x99",                       /* 丙 */
    "\xE4\xb8\x8E",                       /* 丁 */
    "\xE6\x88\x8A",                       /* 戊 */
    "\xE5\xb7\xb1"                        /* 已 */
};

const char Lunar_BranchStrig[12][4] = {
    "\xE7\x94\xB3",                       /* 申 */
    "\xE9\x85\x89",                       /* 酉 */
    "\xE6\x88\x8C",                       /* 戌 */
    "\xE4\xBA\xA5",                       /* 亥 */
    "\xE5\xAD\x90",                       /* 子 */
    "\xE4\xb8\x91",                       /* 丑 */
    "\xE5\xAF\x85",                       /* 寅 */
    "\xE5\x8D\xAF",                       /* 卯 */
    "\xE8\xBE\xB0",                       /* 辰 */
    "\xE5\xb7\xb3",                       /* 巳 */
    "\xE5\x8D\x88",                       /* 午 */
    "\xE6\x9c\xAA"                        /* 未 */
};

const char WEEKCN[8][4] = {
    "\xE6\x97\xA5",                       /* 日 */
    "\xE4\xb8\x80",                       /* 一 */
    "\xE4\xBA\x8C",                       /* 二 */
    "\xE4\xb8\x89",                       /* 三 */
    "\xE5\x9b\x9B",                       /* 四 */
    "\xE4\xba\x94",                       /* 五 */
    "\xE5\x85\xAD",                       /* 六 */
    "\xE6\x97\xA5"                        /* 日 */
};

/* 2000 ~ 2199 */
static const uint32_t lunar_month_days[] = {
    1997,
    0x0000B26D, 0x0000125C, 0x0000192C, 0x00009A95, 0x00001A94, 0x00001B4A, 0x00004B55, 0x00000AD4, 0x0000F55B,
    0x000004BA, 0x0000125A, 0x0000B92B, 0x0000152A, 0x00001694, 0x000096AA, 0x000015AA, 0x00012AB5, 0x00000974,
    0x000014B6, 0x0000CA57, 0x00000A56, 0x00001526, 0x00008E95, 0x00000D54, 0x000015AA, 0x000049B5, 0x0000096C,
    0x0000D4AE, 0x0000149C, 0x00001A4C, 0x0000BD26, 0x00001AA6, 0x00000B54, 0x00006D6A, 0x000012DA, 0x0001695D,
    0x0000095A, 0x0000149A, 0x0000DA4B, 0x00001A4A, 0x00001AA4, 0x0000BB54, 0x000016B4, 0x00000ADA, 0x0000495B,
    0x00000936, 0x0000F497, 0x00001496, 0x0000154A, 0x0000B6A5, 0x00000DA4, 0x000015B4, 0x00006AB6, 0x0000126E,
    0x0001092F, 0x0000092E, 0x00000C96, 0x0000CD4A, 0x00001D4A, 0x00000D64, 0x0000956C, 0x0000155C, 0x0000125C,
    0x0000792E, 0x0000192C, 0x0000FA95, 0x00001A94, 0x00001B4A, 0x0000AB55, 0x00000AD4, 0x000014DA, 0x00008A5D,
    0x00000A5A, 0x0001152B, 0x0000152A, 0x00001694, 0x0000D6AA, 0x000015AA, 0x00000AB4, 0x000094BA, 0x000014B6,
    0x00000A56, 0x00007527, 0x00000D26, 0x0000EE53, 0x00000D54, 0x000015AA, 0x0000A9B5, 0x0000096C, 0x000014AE,
    0x00008A4E, 0x00001A4C, 0x00011D26, 0x00001AA4, 0x00001B54, 0x0000CD6A, 0x00000ADA, 0x0000095C, 0x0000949D,
    0x0000149A, 0x00001A2A, 0x00005B25, 0x00001AA4, 0x0000FB52, 0x000016B4, 0x00000ABA, 0x0000A95B, 0x00000936,
    0x00001496, 0x00009A4B, 0x0000154A, 0x000136A5, 0x00000DA4, 0x000015AC, 0x0000CAB6, 0x0000126E, 0x0000092E,
    0x00008C97, 0x00000A96, 0x00000D4A, 0x00006DA5, 0x00000D54, 0x0000F56A, 0x0000155A, 0x00000A5C, 0x0000B92E,
    0x0000152C, 0x00001A94, 0x00009D4A, 0x00001B2A, 0x00016B55, 0x00000AD4, 0x000014DA, 0x0000CA5D, 0x00000A5A,
    0x0000151A, 0x0000BA95, 0x00001654, 0x000016AA, 0x00004AD5, 0x00000AB4, 0x0000F4BA, 0x000014B6, 0x00000A56,
    0x0000B517, 0x00000D16, 0x00000E52, 0x000096AA, 0x00000D6A, 0x000165B5, 0x0000096C, 0x000014AE, 0x0000CA2E,
    0x00001A2C, 0x00001D16, 0x0000AD52, 0x00001B52, 0x00000B6A, 0x0000656D, 0x0000055C, 0x0000F45D, 0x0000145A,
    0x00001A2A, 0x0000DA95, 0x000016A4, 0x00001AD2, 0x00008B5A, 0x00000AB6, 0x0001455B, 0x000008B6, 0x00001456,
    0x0000D52B, 0x0000152A, 0x00001694, 0x0000B6AA, 0x000015AA, 0x00000AB6, 0x000064B7, 0x000008AE, 0x0000EC57,
    0x00000A56, 0x00000D2A, 0x0000CD95, 0x00000B54, 0x0000156A, 0x00008A6D, 0x0000095C, 0x000014AE, 0x00004A56,
    0x00001A54, 0x0000DD2A, 0x00001AAA, 0x00000B54, 0x0000B56A, 0x000014DA, 0x0000095C, 0x000074AB, 0x0000149A,
    0x0000FA4B, 0x00001652, 0x000016AA, 0x0000CAD5, 0x000005B4
};

/* 2000 ~ 2199 */
static const uint32_t solar_1_1[] = {
    1997,
    0x000F9C3C, 0x000F9E50, 0x000FA045, 0x000FA238, 0x000FA44C, 0x000FA641, 0x000FA836, 0x000FAA49, 0x000FAC3D,
    0x000FAE52, 0x000FB047, 0x000FB23A, 0x000FB44E, 0x000FB643, 0x000FB837, 0x000FBA4A, 0x000FBC3F, 0x000FBE53,
    0x000FC048, 0x000FC23C, 0x000FC450, 0x000FC645, 0x000FC839, 0x000FCA4C, 0x000FCC41, 0x000FCE36, 0x000FD04A,
    0x000FD23D, 0x000FD451, 0x000FD646, 0x000FD83A, 0x000FDA4D, 0x000FDC43, 0x000FDE37, 0x000FE04B, 0x000FE23F,
    0x000FE453, 0x000FE648, 0x000FE83C, 0x000FEA4F, 0x000FEC44, 0x000FEE38, 0x000FF04C, 0x000FF241, 0x000FF436,
    0x000FF64A, 0x000FF83E, 0x000FFA51, 0x000FFC46, 0x000FFE3A, 0x0010004E, 0x00100242, 0x00100437, 0x0010064B,
    0x00100841, 0x00100A53, 0x00100C48, 0x00100E3C, 0x0010104F, 0x00101244, 0x00101438, 0x0010164C, 0x00101842,
    0x00101A35, 0x00101C49, 0x00101E3D, 0x00102051, 0x00102245, 0x0010243A, 0x0010264E, 0x00102843, 0x00102A37,
    0x00102C4B, 0x00102E3F, 0x00103053, 0x00103247, 0x0010343B, 0x0010364F, 0x00103845, 0x00103A38, 0x00103C4C,
    0x00103E42, 0x00104036, 0x00104249, 0x0010443D, 0x00104651, 0x00104846, 0x00104A3A, 0x00104C4E, 0x00104E43,
    0x00105038, 0x0010524A, 0x0010543E, 0x00105652, 0x00105847, 0x00105A3B, 0x00105C4F, 0x00105E45, 0x00106039,
    0x0010624C, 0x00106441, 0x00106635, 0x00106849, 0x00106A3D, 0x00106C51, 0x00106E47, 0x0010703C, 0x0010724F,
    0x00107444, 0x00107638, 0x0010784C, 0x00107A3F, 0x00107C53, 0x00107E48, 0x0010803D, 0x00108250, 0x00108446,
    0x0010863A, 0x0010884E, 0x00108A42, 0x00108C36, 0x00108E4A, 0x0010903E, 0x00109251, 0x00109447, 0x0010963B,
    0x0010984F, 0x00109A43, 0x00109C37, 0x00109E4B, 0x0010A041, 0x0010A253, 0x0010A448, 0x0010A63D, 0x0010A851,
    0x0010AA45, 0x0010AC39, 0x0010AE4D, 0x0010B042, 0x0010B236, 0x0010B44A, 0x0010B63E, 0x0010B852, 0x0010BA47,
    0x0010BC3B, 0x0010BE4F, 0x0010C044, 0x0010C237, 0x0010C44B, 0x0010C641, 0x0010C854, 0x0010CA48, 0x0010CC3D,
    0x0010CE50, 0x0010D045, 0x0010D239, 0x0010D44C, 0x0010D642, 0x0010D837, 0x0010DA4A, 0x0010DC3E, 0x0010DE52,
    0x0010E047, 0x0010E23A, 0x0010E44E, 0x0010E643, 0x0010E838, 0x0010EA4B, 0x0010EC41, 0x0010EE54, 0x0010F049,
    0x0010F23C, 0x0010F450, 0x0010F645, 0x0010F839, 0x0010FA4C, 0x0010FC42, 0x0010FE37, 0x0011004B, 0x0011023E,
    0x00110452, 0x00110647, 0x0011083B, 0x00110A4E, 0x00110C43, 0x00110E38, 0x0011104C, 0x0011123F, 0x00111435,
    0x00111648, 0x0011183C, 0x00111A4F, 0x00111C45, 0x00111E39, 0x0011204D, 0x00112242, 0x00112436, 0x0011264A,
    0x0011283E, 0x00112A51, 0x00112C46, 0x00112E3B, 0x0011304F
};

static uint32_t GetBitInt(uint32_t data, uint8_t length, uint8_t shift)
{
    return (data & (((1U << length) - 1U) << shift)) >> shift;
}

static uint16_t SolarToInt(uint16_t y, uint8_t m, uint8_t d)
{
    m = (m + 9) % 12;
    y = y - m / 10;
    return 365 * y + y / 4 - y / 100 + y / 400 + (m * 306 + 5) / 10 + (d - 1);
}

void LUNAR_SolarToLunar(struct Lunar_Date *lunar,
                         uint16_t solar_year, uint8_t solar_month, uint8_t solar_date)
{
    uint8_t i, lunarM, m, d, leap, dm;
    uint16_t year_index, lunarY, y, offset;
    uint32_t solar_data, solar11, days;

    if (solar_month < 1 || solar_month > 12 || solar_date < 1 || solar_date > 31
        || (solar_year - solar_1_1[0] < 3)
        || ((solar_year - solar_1_1[0]) > (sizeof(solar_1_1) / sizeof(uint32_t) - 2)))
    {
        lunar->Year = 0;
        lunar->Month = 0;
        lunar->Date = 0;
        lunar->IsLeap = 0;
        return;
    }

    year_index = solar_year - solar_1_1[0];
    solar_data = ((uint32_t)solar_year << 9) | ((uint32_t)solar_month << 5) | ((uint32_t)solar_date);
    if (solar_1_1[year_index] > solar_data) {
        year_index -= 1;
    }
    solar11 = solar_1_1[year_index];
    y = GetBitInt(solar11, 12, 9);
    m = GetBitInt(solar11, 4, 5);
    d = GetBitInt(solar11, 5, 0);
    offset = SolarToInt(solar_year, solar_month, solar_date) - SolarToInt(y, m, d);

    days = lunar_month_days[year_index];
    leap = GetBitInt(days, 4, 13);

    lunarY = year_index + solar_1_1[0];
    lunarM = 1;
    offset += 1;
    for (i = 0; i < 13; i++) {
        if (GetBitInt(days, 1, 12 - i) == 1) {
            dm = 30;
        } else {
            dm = 29;
        }
        if (offset > dm) {
            lunarM += 1;
            offset -= dm;
        } else {
            break;
        }
    }
    lunar->IsLeap = 0;
    if (leap != 0 && lunarM > leap) {
        if (lunarM == leap + 1) {
            lunar->IsLeap = 1;
        }
        lunarM -= 1;
    }
    lunar->Month = lunarM;
    lunar->Date = offset;
    lunar->Year = lunarY;
}

uint8_t LUNAR_GetZodiac(const struct Lunar_Date *lunar)
{
    return lunar->Year % 12;
}

uint8_t LUNAR_GetStem(const struct Lunar_Date *lunar)
{
    return lunar->Year % 10;
}

uint8_t LUNAR_GetBranch(const struct Lunar_Date *lunar)
{
    return lunar->Year % 12;
}
