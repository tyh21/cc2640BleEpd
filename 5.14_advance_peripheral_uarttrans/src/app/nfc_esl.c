/******************************************************************************

 @file  nfc_esl.c

 @brief FM11NC08S NFC channel + ISO14443-4 PICC responder for the ESL tag.

   Ported from the hardware-proven bare-metal responder
   (cc2640Ble/5.14_advance_peripheral_uarttrans/src/app/main.c, v38) into
   this BLE application, so the price tag can be updated over EITHER
   channel:

       BLE  -> SPPBLE / task_epd.c   (frame commands, existing path)
       NFC  -> this file             (ISO-DEP APDUs, phone tap)

   What changed relative to the bare-metal original:

     1. It is a TI-RTOS task now, not a loop that runs before BIOS_start.
        So the panel is driven through the existing EPD_2IN13_* driver
        (SPI0 + PIN driver) instead of a private bit-bang copy - the pins
        are identical anyway (SCLK 18 / MOSI 19 / CS 12 / DC 11 / RST 10 /
        BUSY 9 / PWR 20), and sharing the driver means the two producers
        can never fight over the SPI handle.
     2. FM11NC08S side stays bit-banged: it is an ISR-free, register-level
        protocol that the driver stack does not cover, and the bit-bang
        version is the one with a confirmed-good log behind it.
     3. The log is buffered and drained in small slices.  v33's lesson:
        one 900-character flush is 78 ms of blocked UART inside an
        exchange whose frame waiting time is 9.7 ms.

   Protocol facts baked in (all confirmed on hardware against a real
   Android phone):

     - The chip appends CRC_A itself.  We write PCB + INF only.  (Writing
       our own CRC is how v30 produced a bogus F1 09 status word.)
     - APDU answers are decided by the APDU: F0 DE / F0 D8 are case-2 with
       Le=1 and must return one data byte (00 = idle, 01 = busy) before
       90 00.  Everything else returns 90 00.
     - A chained I-block (chn=1) is acknowledged with an R(ACK); the real
       answer goes out for the chn=0 block.
     - R(NAK)#n follows ISO/IEC 14443-4 rules 11/12: n == our current
       block -> retransmit the last I-block; n != ours -> R(ACK).
     - The steady ~650 ms "B2 67 C7" tail after a session is the reader's
       block-presence check, not a failure.  Answering it correctly every
       time IS the fix; there is nothing to escape from.

 SAFETY: no EEPROM write at all.  No opcode above 0xBF is sent.

 *****************************************************************************/

#include <string.h>

#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/BIOS.h>

#include <ti/drivers/PIN.h>
#include <ti/drivers/pin/PINCC26XX.h>

#include <ti/devices/cc26x0r2/driverlib/ioc.h>
#include <ti/devices/cc26x0r2/driverlib/gpio.h>

#include "nfc_esl.h"
#include "epd2in13.h"
#include "epd_clock.h"     /* display mode: keep a pushed image on screen */
#include "task_epd.h"
#include "hw_uart.h"

/* ======================= FM11NC08S wiring =======================
 * doc/pins.txt: the NFC channel chip is wired to IOID_4/5/6/16.
 * IOID_6 doubles as the red LED on boards that have one - the NFC clock
 * wins, which is why the red LED stops blinking once NFC is enabled.
 * =============================================================== */
#define FM_SSN   IOID_4
#define FM_SCLK  IOID_6
#define FM_MOSI  IOID_5
#define FM_MISO  IOID_16

/* Bit-bang pacing, in volatile-loop iterations at 48 MHz.  Tuned to sit
 * at the same order of magnitude as the bare-metal build that worked
 * (dly=4 / settle=150), with a little extra slack for interrupt jitter -
 * SPI has no lower clock bound, so slower is only ever safer. */
static uint32_t g_fm_edge   = 4u;    /* half a SCLK period      */
static uint32_t g_fm_settle = 60u;   /* CS low -> first edge    */
static uint32_t g_fm_swait  = 30u;   /* CS high -> next CS low  */

static void fm_delay(uint32_t n)
{
    volatile uint32_t k = n;
    while (k != 0u) { k--; }
}

/* ======================= buffered logging =======================
 * RAM IS THE SCARCE RESOURCE HERE.  This application shares the
 * CC2640R2F's 20 KB SRAM with the BLE5 stack, and 3 KB of it is
 * reserved for the ROM stack, so everything the app declares comes
 * straight out of the ICall heap the BLE stack allocates from.
 * The first integration used a 512-byte log buffer and a 1536-byte
 * task stack; that shrank the heap to ~2.1 KB, the BLE stack failed
 * to allocate during start-up, and the assert spin-lock wedged the
 * whole device (no BLE, no clock, no NFC).  The numbers below are
 * deliberately small - the log only has to be readable, not roomy. */
#define LGMAX     96
#define LG_DRAIN  24          /* ~2.1 ms at 115200 - inside one FWT */

static char     g_lg[LGMAX];
static uint16_t g_lg_n    = 0;
static uint32_t g_lg_drop = 0;

static void lg_ch(char c)
{
    if (g_lg_n < (LGMAX - 1u)) g_lg[g_lg_n++] = c;
    else                       g_lg_drop++;
}

static void lg_s(const char *s) { while (*s) lg_ch(*s++); }

static void lg_hex(uint8_t v)
{
    const char h[] = "0123456789ABCDEF";
    lg_ch(h[(v >> 4) & 15]);
    lg_ch(h[v & 15]);
}

static void lg_dec(uint32_t v)
{
    char b[12]; uint8_t i = 0;
    if (v == 0u) { lg_ch('0'); return; }
    while (v) { b[i++] = (char)('0' + (uint8_t)(v % 10u)); v /= 10u; }
    while (i) lg_ch(b[--i]);
}

/* write the whole buffer - only when the link is known to be idle */
static void lg_flush_all(void)
{
    if (g_lg_n != 0u) {
        HWUART_Write((const uint8_t *)g_lg, g_lg_n);
        g_lg_n = 0u;
    }
    if (g_lg_drop != 0u) {
        HWUART_Printf("[log dropped %u chars]\r\n", (unsigned)g_lg_drop);
        g_lg_drop = 0u;
    }
}

/* write at most LG_DRAIN characters, so a drain can never outlast the
 * reader's frame waiting time */
static void lg_drain(void)
{
    uint16_t k = (g_lg_n > (uint16_t)LG_DRAIN) ? (uint16_t)LG_DRAIN : g_lg_n;

    if (k == 0u) return;
    HWUART_Write((const uint8_t *)g_lg, k);
    if (k < g_lg_n) {
        memmove(g_lg, g_lg + k, (size_t)(g_lg_n - k));
    }
    g_lg_n = (uint16_t)(g_lg_n - k);
}

/* ======================= bit-banged SPI (mode 0) ======================= */
static void fm_pins_init(void)
{
    IOCPinTypeGpioOutput(FM_SSN);
    IOCPinTypeGpioOutput(FM_SCLK);
    IOCPinTypeGpioOutput(FM_MOSI);
    IOCPinTypeGpioInput(FM_MISO);

    GPIO_setDio(FM_SSN);
    GPIO_clearDio(FM_SCLK);
    GPIO_clearDio(FM_MOSI);
}

static uint8_t fm_spi_tx(uint8_t b)
{
    uint8_t rx = 0, i;

    GPIO_clearDio(FM_SCLK);                     /* mode 0 idle level */
    for (i = 0; i < 8u; i++) {
        if ((b & 0x80u) != 0u) GPIO_setDio(FM_MOSI);
        else                   GPIO_clearDio(FM_MOSI);
        b = (uint8_t)(b << 1);

        fm_delay(g_fm_edge);
        GPIO_setDio(FM_SCLK);                   /* sample on rising  */
        fm_delay(g_fm_edge);
        rx = (uint8_t)((rx << 1) | (GPIO_readDio(FM_MISO) & 1u));
        GPIO_clearDio(FM_SCLK);
    }
    return rx;
}

#define FM_XBUF 56       /* one APDU chain block is at most 48 B */
static uint8_t g_fm_tx[FM_XBUF];
static uint8_t g_fm_rx[FM_XBUF];

static void fm_xfer(const uint8_t *tx, uint16_t n, uint8_t *rx)
{
    uint16_t i;

    GPIO_clearDio(FM_SSN);
    fm_delay(g_fm_settle);
    for (i = 0; i < n; i++) rx[i] = fm_spi_tx(tx[i]);
    fm_delay(20u);
    GPIO_clearDio(FM_SCLK);
    GPIO_setDio(FM_SSN);
    fm_delay(g_fm_swait);
}

/* opcode = base | 0x20 -> read ; opcode = base -> write
 * base 0x00 registers | 0x80 FIFO write | 0xA0 FIFO read.
 * Data always starts at rx[1] (slot 0 is the opcode slot). */
static void fm_rd_op(uint8_t op, uint8_t *buf, uint16_t n)
{
    uint16_t i;
    g_fm_tx[0] = op;
    for (i = 0; i < n; i++) g_fm_tx[1 + i] = 0x00u;
    fm_xfer(g_fm_tx, (uint16_t)(1u + n), g_fm_rx);
    for (i = 0; i < n; i++) buf[i] = g_fm_rx[1 + i];
}

static void fm_wr_op(uint8_t op, const uint8_t *d, uint8_t n)
{
    uint8_t i;
    g_fm_tx[0] = op;
    for (i = 0; i < n; i++) g_fm_tx[1 + i] = d[i];
    fm_xfer(g_fm_tx, (uint16_t)(1u + n), g_fm_rx);
}

static void fm_reg_rd(uint8_t r, uint8_t *buf, uint16_t n) { fm_rd_op((uint8_t)(r | 0x20u), buf, n); }
static void fm_reg_wr(uint8_t r, const uint8_t *d, uint8_t n) { fm_wr_op(r, d, n); }

/* ======================= ISO14443-4 helpers ======================= */
/* CRC_A: poly 0x8408, init 0x6363 */
static uint16_t crc_a(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0x6363u, i;
    uint8_t  j;

    for (i = 0; i < n; i++) {
        crc ^= d[i];
        for (j = 0; j < 8u; j++) {
            if ((crc & 1u) != 0u) crc = (uint16_t)((crc >> 1) ^ 0x8408u);
            else                  crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static uint8_t crc_ok(const uint8_t *f, uint8_t n)
{
    uint16_t e;
    if (n < 3u) return 0u;
    e = crc_a(f, (uint16_t)(n - 2u));
    return (uint8_t)((f[n - 2u] == (uint8_t)(e & 0xFFu)) &&
                     (f[n - 1u] == (uint8_t)(e >> 8)));
}

/* ======================= responder state ======================= */
static volatile uint8_t g_nfc_busy = 0u;     /* exchange / transfer open */

static uint32_t s_rx_cnt = 0u, s_tx_cnt = 0u, s_crc_bad = 0u;
static uint32_t s_att = 0u, s_nak_rcv = 0u, s_rule11 = 0u, s_rule12 = 0u;
static uint32_t s_chain_fr = 0u, s_chain_apdu = 0u;
static uint8_t  s_blk      = 0u;             /* mirrors the reader       */
static uint8_t  s_prev_blk = 0xFFu;
static uint8_t  s_in_chain = 0u;
static uint8_t  s_ch_live  = 0u;

static uint8_t  s_tx_blk    = 0u;            /* our last I-block number  */
static uint8_t  s_last_inf[8];               /* its INF, for rule 11     */
static uint8_t  s_last_inf_n = 0u;

#define CH_HEAD 8
static uint8_t  s_ch[CH_HEAD];
static uint8_t  s_ch_n   = 0u;
static uint16_t s_ch_len = 0u;

/* ---- panel / image streaming ---- */
#define EPD_PKT_BYTES 250u                   /* payload of one F0 D2     */
#define EPD_HDR         5u                   /* F0 D2 P1 P2 Lc           */

static uint8_t  s_panel_ready = 0u;
static uint8_t  s_img_on      = 0u;
static uint8_t  s_img_seq     = 0u;
static uint32_t s_img_bytes   = 0u;
static uint16_t s_img_pkts    = 0u;
static uint8_t  s_img_hold    = 0u;          /* clock refresh held off   */
static uint32_t s_img_idle    = 0u;

static uint8_t  s_redraw    = 0u;
static uint32_t s_redraws   = 0u;
static volatile uint8_t g_epd_busy = 0u;     /* 00 = idle, 01 = busy     */
static uint8_t  s_epd_high  = 0u;
static uint32_t s_epd_low   = 0u;            /* ms seen with BUSY released */
static uint32_t s_epd_wait  = 0u;
/* These are all in MILLISECONDS: the loop accumulates elapsed ms (see
 * nfc_now_ms / "slots"), so they keep their meaning at any poll period. */
#define EPD_WAIT_MAX     3000u               /* 3 s with no BUSY edge    */
#define IMG_HOLD_MAX    10000u               /* 10 s to drop the hold    */
#define CH_STALL_SLOTS    300u               /* 300 ms mid-chain         */
#define PANEL_RETRY_SLOTS 8000u              /* 8 s between panel retries:
                                              * an attempt itself costs up to
                                              * 2 s, so this keeps the
                                              * responder polling at least
                                              * 80% of the time even with no
                                              * panel attached at all      */

/* ---------- put PCB + INF on the air ----------
 * The FM11NC08S appends CRC_A itself, so we must NOT append it. */
static void tx_pcb(const uint8_t *inf, uint8_t inf_len, uint8_t pcb)
{
    uint8_t fr[40];
    uint8_t n = (uint8_t)(inf_len + 1u);
    uint8_t one = 1u, i;

    fr[0] = pcb;
    for (i = 0; i < inf_len; i++) fr[1 + i] = inf[i];

    fm_reg_wr(0x01u, &one, 1);               /* FIFO_FLUSH               */
    fm_wr_op(0x80u, fr, n);                  /* FIFO write               */
    { const uint8_t t = 0x55u; fm_reg_wr(0x04u, &t, 1); }   /* RF_TXEN    */
    s_tx_cnt++;
}

static void tx_rack(void)
{
    tx_pcb((const uint8_t *)0, 0u, (uint8_t)(0xA2u | s_blk));
}

static void tx_rack_n(uint8_t num)
{
    tx_pcb((const uint8_t *)0, 0u, (uint8_t)(0xA2u | (num & 1u)));
}

/* retransmit our last I-block, rebuilt from s_last_inf so that an R(ACK)
 * sent for a chain block can never clobber the answer */
static void tx_resend_i(uint8_t blk)
{
    uint8_t fr[16], i;

    if (s_last_inf_n == 0u) { tx_rack(); return; }
    blk &= 1u;
    fr[0] = (uint8_t)(0x02u | blk);
    for (i = 0; i < s_last_inf_n; i++) fr[1 + i] = s_last_inf[i];

    {
        uint8_t one = 1u;
        fm_reg_wr(0x01u, &one, 1);
        fm_wr_op(0x80u, fr, (uint8_t)(s_last_inf_n + 1u));
        { const uint8_t t = 0x55u; fm_reg_wr(0x04u, &t, 1); }
        s_tx_cnt++;
    }
}

/* ---------- the answer an APDU asks for ----------
 * F0 D4 REDRAW answers immediately (P2 bit 7 asks for exactly that) and
 * the refresh is kicked off from the polling loop, because a full 2.13"
 * update takes seconds - far longer than the reader would wait and far
 * longer than this path may block.
 * F0 DE / F0 D8 are case-2 with Le=1: one data byte, then 90 00. */
static uint8_t build_resp(uint8_t *out)
{
    uint8_t cla = (s_ch_n >= 2u) ? s_ch[0] : 0x00u;
    uint8_t ins = (s_ch_n >= 2u) ? s_ch[1] : 0x00u;

    if (cla == 0xF0u && ins == 0xD4u) {
        s_redraw   = 1u;
        g_epd_busy = 0x01u;      /* tell the truth from the first F0 DE */
        s_epd_high = 0u;
        s_epd_low  = 0u;
        s_epd_wait = 0u;
    }

    if (cla == 0xF0u && (ins == 0xDEu || ins == 0xD8u)) {
        out[0] = g_epd_busy;     /* 00 = idle, 01 = busy                 */
        out[1] = 0x90u;
        out[2] = 0x00u;
        return 3u;
    }

    /* F0 D5 P1 P2 Le - display mode (the app's 时钟/图片 switch)
     *   F0 D5 00 00 01   set clock mode
     *   F0 D5 00 01 01   set image mode
     *   F0 D5 01 00 01   read the mode
     * P1 picks set (00) or get (01); the answer is one data byte with the
     * mode that is now in effect, then 90 00 - same shape as F0 DE. */
    if (cla == 0xF0u && ins == 0xD5u) {
        uint8_t p1 = (s_ch_n >= 3u) ? s_ch[2] : 0x00u;
        uint8_t p2 = (s_ch_n >= 4u) ? s_ch[3] : 0x00u;

        if (p1 == 0x00u) {
            /* handing the screen back to the clock must also drop every
             * "hold the clock off" flag, otherwise a stale hold left behind
             * by an aborted tap keeps the tick suppressed and the calendar
             * never comes back */
            if (p2 == 0x00u) NfcEsl_yieldPanel();
            epd_clock_set_mode(p2);
        }
        out[0] = epd_clock_get_mode();
        out[1] = 0x90u;
        out[2] = 0x00u;
        return 3u;
    }

    out[0] = 0x90u;
    out[1] = 0x00u;
    return 2u;
}

/* ---------- one chain block of an APDU -> panel RAM ----------
 * Only F0 D2 ... FA is streamed; everything else is left alone.  Called
 * for every chained block, so the image arrives in ~29-byte pieces and
 * no image buffer is ever needed. */
static void img_stream_block(const uint8_t *inf, uint8_t len, uint16_t base)
{
    uint16_t from, end;

    if (base == 0u) {                                  /* APDU head */
        s_img_on = 0u;
        if (len >= EPD_HDR && inf[0] == 0xF0u && inf[1] == 0xD2u &&
            inf[4] == (uint8_t)EPD_PKT_BYTES) {
            s_img_on  = 1u;
            s_img_seq = inf[3];                        /* P2 = packet index */
            if (s_img_seq == 0u) {                     /* fresh image       */
                s_img_pkts  = 0u;
                s_img_bytes = 0u;
            }
            s_img_pkts++;
            s_img_idle = 0u;
            if (s_img_hold == 0u) {
                s_img_hold = 1u;
                EPDTask_setImageTransfer(1);           /* pause the clock   */
                lg_s("LOAD IMAGE -> panel RAM\r\n");
            }
        }
    }
    if (s_img_on == 0u) return;

    from = (base > EPD_HDR) ? base : EPD_HDR;
    end  = (uint16_t)(base + len);
    if (end > 255u) end = 255u;                        /* APDU is 255 B     */
    if (end <= from) return;

    if (s_panel_ready != 0u) {
        /* pos = P2 * 250 + offset inside the packet; the driver re-arms the
         * RAM address on every call, so a retransmitted block is harmless.
         * Take the panel lock so a BLE/clock render cannot interleave. */
        EPD_Panel_Acquire();
        EPD_2IN13_WriteRAMAt((uint32_t)s_img_seq * EPD_PKT_BYTES +
                             (uint32_t)(from - EPD_HDR),
                             &inf[from - base], (int)(end - from));
        EPD_Panel_Release();
    } else if (s_img_seq == 0u && from == EPD_HDR) {
        /* Only once per image, or the tiny log buffer overflows instantly.
         * Visible on purpose: silent data loss is what made the dead-panel
         * case take so long to find. */
        lg_s("!panel not ready - image dropped\r\n");
    }
    s_img_bytes += (uint32_t)(end - from);
}

/* ---------- one frame arrived: classify it and act ---------- */
static void on_frame(const uint8_t *g, uint8_t n)
{
    uint8_t pcb, i;

    /* the FIFO also spits residue while the field is on but no valid frame
     * has arrived - anything whose CRC_A does not check out is not a reader
     * frame, so count it and stay quiet */
    if (n < 3u) { s_rx_cnt++; s_crc_bad++; return; }
    s_rx_cnt++;
    if (!crc_ok(g, n)) { s_crc_bad++; return; }

    g_nfc_busy = 1u;
    s_img_idle = 0u;                 /* the phone is alive - keep the hold */

    pcb = g[0];

    /* ---- chained I-block: R(ACK) with the same block number ---- */
    if (((pcb & 0xC0u) == 0x00u) && ((pcb & 0x10u) != 0u)) {
        uint8_t L    = (uint8_t)(n - 3u);
        uint8_t nnew = (uint8_t)((pcb & 1u) != s_prev_blk);

        s_blk      = (uint8_t)(pcb & 1u);
        s_prev_blk = s_blk;
        if (nnew) {
            img_stream_block(&g[1], L, s_ch_len);
            for (i = 0; i < L; i++) {
                if (s_ch_n < CH_HEAD) { s_ch[s_ch_n] = g[1 + i]; s_ch_n++; }
            }
            s_ch_len = (uint16_t)((s_ch_len + L > 0xFFF0u) ? 0xFFF0u
                                                           : (s_ch_len + L));
            s_img_idle = 0u;
        }
        s_chain_fr++;
        s_ch_live++;
        s_in_chain  = 1u;
        g_nfc_busy  = 1u;
        lg_ch('.');
        tx_rack();
        return;
    }

    /* ---- final or standalone I-block: the complete APDU ---- */
    if ((pcb & 0xC0u) == 0x00u) {
        uint8_t L    = (uint8_t)(n - 3u);
        uint8_t nnew = (uint8_t)((pcb & 1u) != s_prev_blk);

        s_blk = (uint8_t)(pcb & 1u);
        s_prev_blk = s_blk;

        if (nnew) {
            img_stream_block(&g[1], L, s_ch_len);
            for (i = 0; i < L; i++) {
                if (s_ch_n < CH_HEAD) { s_ch[s_ch_n] = g[1 + i]; s_ch_n++; }
            }
            s_ch_len = (uint16_t)((s_ch_len + L > 0xFFF0u) ? 0xFFF0u
                                                           : (s_ch_len + L));
        }
        s_in_chain = 0u;
        s_chain_apdu++;

        lg_s("\r\nAPDU "); lg_dec(s_ch_len); lg_s("B ");
        if (s_ch_n >= 2u) { lg_hex(s_ch[0]); lg_ch(' '); lg_hex(s_ch[1]); }
        else              { lg_s("-- --"); }
        if (s_ch_live > 0u) { lg_s(" ch="); lg_dec(s_ch_live); }
        lg_s(" -> ");

        {
            uint8_t ans[8];
            uint8_t k = build_resp(ans);
            lg_dec(k); lg_s("B :");
            for (i = 0; i < k; i++) { lg_ch(' '); lg_hex(ans[i]); }
            lg_s("\r\n");

            s_att++;
            for (i = 0; i < k; i++) s_last_inf[i] = ans[i];
            s_last_inf_n = k;
            s_tx_blk = (uint8_t)(s_blk & 1u);
            tx_pcb(ans, k, (uint8_t)(0x02u | s_blk));
        }
        s_ch_n = 0u; s_ch_len = 0u; s_ch_live = 0u;
        return;
    }

    /* ---- R-block ---- */
    if ((pcb & 0xC0u) == 0x80u) {
        uint8_t an  = (uint8_t)((pcb >> 4) & 1u);
        uint8_t num = (uint8_t)(pcb & 1u);

        if (!an) {
            /* R(ACK): the reader took our block */
            g_nfc_busy = 0u;
            return;
        }

        /* R(NAK)#num - ISO/IEC 14443-4 gives a PICC exactly two rules and
         * ONE comparison picks between them:
         *   rule 11  num == our current block -> retransmit the last I-block
         *   rule 12  num != ours              -> R(ACK) carrying OUR number
         * The same two rules answer both a real retransmission request and
         * the reader's post-session block-presence check. */
        s_nak_rcv++;
        if (num == (uint8_t)(s_tx_blk & 1u)) {
            s_rule11++;
            tx_resend_i(num);
        } else {
            s_rule12++;
            tx_rack_n(s_tx_blk);
        }
        return;
    }

    /* S-block: nothing to do */
}

/* ======================= panel bring-up =======================
 * BOUNDED ON PURPOSE.  The first integration called EPD_2IN13_Init() here
 * and the panel answered BUSY-HIGH for 90 s, which meant this task never
 * reached its banner, never polled the FM11NC08S and never answered a tap
 * for a minute and a half after every reset.  Now:
 *
 *   - the responder is announced and polled FIRST,
 *   - a bring-up attempt is a couple of seconds at worst (Init uses a 2 s
 *     BUSY budget),
 *   - if the panel does not answer the attempt is simply repeated from the
 *     polling loop, so a late panel only delays the first tap.
 */
static uint8_t panel_try_init(void)
{
    uint8_t ok;

    if (s_panel_ready != 0u) return 1u;

    EPD_Panel_Acquire();
    epd_hw_init();                 /* idempotent - shared with task_epd.c */

    EPD_2IN13_Init();              /* reset, LUT, RAM window (16B x 250)   */

    if (EPD_2IN13_IsBusy() != 0u) {
        ok = 0u;                   /* panel still not answering            */
    } else {
        /* start every session from a clean white window, so any area the
         * phone does not cover is white and not whatever was there before */
        EPD_2IN13_PrepareBlkRAM(); /* 4000 x 0xFF, address re-armed at 0   */
        s_panel_ready = 1u;
        ok = 1u;
    }
    EPD_Panel_Release();
    return ok;
}

/* ======================= the task =======================
 * PRIORITY 1, not 2.  The EPD/clock task owns priority 2 and this
 * task only polls a bit-banged SPI once a millisecond, so it has no
 * business competing with the display.  Priority 1 also sits below
 * the BLE profile task, which means a tap can never delay the stack.
 *
 * STACK 512: the deepest path is task frame -> on_frame -> build_resp
 * / tx_pcb (fr[40]) -> fm_wr_op -> fm_xfer -> fm_spi_tx, roughly
 * 250-350 bytes of true stack.  512 leaves comfortable headroom while
 * keeping the BLE5 heap intact - see the note on LGMAX above. */
#define NFC_TASK_PRIORITY    1
#define NFC_TASK_STACK_SIZE  512
/* Poll period of the bit-banged FM11NC08S.  1 ms keeps the responder inside the
 * reader's 9.7 ms frame waiting time with comfortable margin.  Raising it (e.g.
 * build with -DNFC_POLL_MS=2) halves the idle wake-ups at the cost of that
 * margin - keep it well under 9 ms.  Overridable from the command line. */
#ifndef NFC_POLL_MS
#define NFC_POLL_MS          1u
#endif
#define LOOP_IDLE_FLUSH      400u   /* ms of quiet before flushing the log */

/* ---- IRQ_N wake-up ----
 * The FM11NC08S IRQ_N output (active low) is wired to DIO_13 on this board.
 * A falling-edge interrupt on it wakes the responder the instant the chip has
 * something -- VERIFIED ON HARDWARE (chip asserts IRQ_N by default, no
 * SYS_CTRL/INT_MASK setup needed).  The poll below is therefore only a safety
 * net for a missed edge: 20 ms keeps a responsive heartbeat while cutting the
 * idle wake-ups ~20x.  Override with -DNFC_FALLBACK_MS=... */
#ifndef NFC_IRQ_PIN
#define NFC_IRQ_PIN          IOID_13
#endif
/* 0 = no fallback poll at all: the task blocks in Event_pend until IRQ_N wakes
 * it.  Non-zero = also wake every NFC_FALLBACK_MS ms as a safety net AND to run
 * the periodic housekeeping in the loop (g_nfc_busy decay, panel retry, log
 * flush).  IRQ-only is the lowest idle power, but then that housekeeping never
 * runs - g_nfc_busy would stick at 1 after the first tap and freeze the clock -
 * so the default keeps a 20 ms housekeeping tick. */
#ifndef NFC_FALLBACK_MS
#define NFC_FALLBACK_MS      200u  /* IRQ wake + 200 ms housekeeping tick */
#endif
#define NFC_EVENT_IRQ        Event_Id_00

static Task_Struct NfcTask;
static Char NfcTaskStack[NFC_TASK_STACK_SIZE];

/* ms the current ISO14443 chain has been stalled (no frame) - resync once it
 * passes CH_STALL_SLOTS */
static uint32_t s_idle_cnt = 0u;

/* ---- IRQ_N (DIO_13) wake-up plumbing ---- */
static Event_Struct NfcEvent;
static Event_Handle hNfcEvent = NULL;

static PIN_State  irqPinState;
static PIN_Handle irqPinHandle = NULL;

static uint32_t s_irq_wakes = 0u;

/* PIN driver interrupt callback.  Runs in Hwi context - do nothing but post. */
static void nfc_irq_cb(PIN_Handle handle, PIN_Id pinId)
{
    (void)handle; (void)pinId;
    if (hNfcEvent != NULL) {
        Event_post(hNfcEvent, NFC_EVENT_IRQ);
    }
}

/* Bring up the IRQ_N input.  If the pin cannot be opened we say so and carry
 * on - the fallback poll still keeps the responder alive on its own. */
static void nfc_irq_init(void)
{
    PIN_Config cfg[] = {
        NFC_IRQ_PIN | PIN_INPUT_EN | PIN_PULLUP | PIN_HYSTERESIS | PIN_IRQ_NEGEDGE,
        PIN_TERMINATE
    };
    Event_Params ep;

    Event_Params_init(&ep);
    Event_construct(&NfcEvent, &ep);
    hNfcEvent = Event_handle(&NfcEvent);

    irqPinHandle = PIN_open(&irqPinState, cfg);
    if (irqPinHandle != NULL) {
        PIN_registerIntCb(irqPinHandle, nfc_irq_cb);
        PIN_setInterrupt(irqPinHandle, NFC_IRQ_PIN | PIN_IRQ_NEGEDGE);
        HWUART_Printf("NFC IRQ_N armed on IOID_%u (falling edge)\r\n",
                      (unsigned)NFC_IRQ_PIN);
    } else {
        HWUART_Printf("NFC IRQ_N pin open FAILED - poll only\r\n");
    }
}

/* ---- real-time loop timing ----
 * The loop spins at a variable rate (fast while an exchange is in progress,
 * once every NFC_FALLBACK_MS when idle), so the housekeeping timers accumulate
 * elapsed MILLISECONDS instead of counting iterations.  At the old 1 ms
 * cadence one iteration was one millisecond, so this is the previous
 * behaviour exactly; it lets the idle poll period be raised without stretching
 * (and thus breaking) those timeouts. */
static uint32_t s_loop_ms = 0u;

static uint32_t nfc_now_ms(void)
{
    return (uint32_t)(Clock_getTicks() / (1000u / Clock_tickPeriod));
}

/* ICall's own free-heap counter, exported by icall.o.  Printing it at
 * start-up is the one number that tells us whether the BLE5 stack has
 * enough room to come up - it needs roughly 3-4 KB, and the whole
 * device wedges if it does not get it. */
extern uint32_t heapmgrMemFreeTotal;

static void NfcEsl_taskFxn(UArg a0, UArg a1)
{
    uint8_t  one = 1u;
    uint32_t idle = 0u;
    uint32_t panel_retry = 0u;

    (void)a0; (void)a1;

    /* let the EPD task finish its own bring-up: it opens the pins and the
     * SPI handle, and its 1-second clock must be constructed before we
     * start claiming the panel */
    Task_sleep(100u * 1000u / Clock_tickPeriod);

    /* ---- get the NFC channel up FIRST ----
     * Nothing below this point may depend on the panel: the responder has
     * to be able to answer a tap even if no panel is attached at all. */
    fm_pins_init();
    nfc_irq_init();                       /* DIO_13 <- IRQ_N (falling)    */
    fm_reg_wr(0x01u, &one, 1);            /* FIFO_FLUSH                  */
    fm_delay(4000u);

    HWUART_Printf("\r\n");
    HWUART_Printf("=== NFC ESL responder ready (FM11NC08S) ===\r\n");
    HWUART_Printf("ICall heap free=%u B  tap the phone\r\n",
                  (unsigned)heapmgrMemFreeTotal);
    lg_s("NFC ESL responder ready\r\n");
    lg_flush_all();

    /* ---- then the panel, in the background and bounded ---- */
    if (panel_try_init() != 0u) {
        HWUART_Printf("panel ready=1  busy stalls=%u\r\n",
                      (unsigned)EPD_2IN13_BusyStalls());
        lg_s("panel ready\r\n");
        lg_flush_all();
    } else {
        HWUART_Printf("panel ready=0  busy stalls=%u - retrying\r\n",
                      (unsigned)EPD_2IN13_BusyStalls());
        lg_s("panel bring-up deferred - retrying\r\n");
        lg_flush_all();
    }

    s_loop_ms = nfc_now_ms();

    while (1) {
        uint8_t irq = 0u;
        uint8_t cnt = 0u;
        uint32_t slots;
        uint32_t now = nfc_now_ms();

        /* housekeeping timers count elapsed ms, not iterations, so they keep
         * their meaning whatever the idle poll period is */
        slots = now - s_loop_ms;
        s_loop_ms = now;
        if (slots == 0u) slots = 1u;

        /* keep trying until the panel answers (every ~8 s of idle time) */
        if (s_panel_ready == 0u && (panel_retry += slots) > PANEL_RETRY_SLOTS) {
            panel_retry = 0u;
            if (panel_try_init() != 0u) {
                lg_s("panel ready now\r\n");
            }
        }

        fm_reg_rd(0x07u, &irq, 1);                     /* INT_STATUS      */
        if ((irq & 0x10u) != 0u) {                     /* RX_DONE         */
            fm_reg_rd(0x02u, &cnt, 1);                 /* FIFO word count */
            if (cnt > 0u && cnt <= 48u) {
                uint8_t f[48];
                fm_rd_op(0xA0u, f, cnt);               /* FIFO read       */
                on_frame(f, cnt);
                idle = 0u;
                g_nfc_busy = 1u;
            } else {
                idle += slots;
            }
        } else {
            idle += slots;
        }

        /* a chain must never wedge us: if the reader walks away mid-APDU,
         * drop the partial APDU and start clean */
        if (s_in_chain != 0u) {
            if ((s_idle_cnt += slots) > CH_STALL_SLOTS) {
                s_in_chain = 0u; s_ch_n = 0u; s_ch_len = 0u; s_ch_live = 0u;
                s_prev_blk = 0xFFu; s_idle_cnt = 0u;
                lg_s("(chain stalled - resync)\r\n");
            }
        } else {
            s_idle_cnt = 0u;
        }

        /* ---- the panel ----
         * F0 D4 asked for a redraw: kick it off HERE, never in the response
         * path.  0x22 0xC7 0x20 is three bytes and returns at once, so the
         * loop keeps answering F0 DE while the panel works.  BUSY decides
         * when the answer may go back to 00. */
        if (s_redraw != 0u) {
            s_redraw = 0u;
            s_redraws++;
            EPD_Panel_Acquire();
            EPD_2IN13_UpdateDisplayAsync();
            EPD_Panel_Release();
            g_epd_busy = 0x01u;
            s_epd_high = 0u; s_epd_low = 0u; s_epd_wait = 0u;
            lg_s("F0 D4 -> refresh started (");
            lg_dec(s_img_bytes); lg_s("B, ");
            lg_dec((uint32_t)s_img_pkts); lg_s(" packets)\r\n");
        } else if (g_epd_busy != 0u) {
            if (EPD_2IN13_IsBusy() != 0u) {
                s_epd_high = 1u;
                s_epd_low  = 0u;
            } else if (s_epd_high != 0u) {
                if ((s_epd_low += slots) >= 4u) {      /* debounce release */
                    /* Lock the display into image mode BEFORE F0 DE is
                     * allowed to answer 00, so the app can never read
                     * "idle" while the panel is still clock-owned. */
                    epd_clock_set_mode(EPD_MODE_IMAGE);
                    g_epd_busy = 0u;
                    s_epd_low  = 0u;
                    /* drop the analog current now that nobody needs it, but
                     * keep VCC + RAM so the next tap can just refresh */
                    EPD_Panel_Acquire();
                    EPD_2IN13_SleepLight();
                    EPD_Panel_Release();
                    /* the picture is on the panel and stays there: the
                     * 1-minute clock tick may not paint over it.  This is
                     * the sticky part - the old code only held the clock
                     * for 10 s and then let it take the screen back. */
                    lg_s("panel idle - F0 DE answers 00, mode=image\r\n");
                    if (s_img_hold != 0u) {
                        s_img_hold = 0u;
                        EPDTask_setImageTransfer(0);
                    }
                }
            } else if ((s_epd_wait += slots) > EPD_WAIT_MAX) {
                g_epd_busy = 0u;                       /* no panel attached */
                s_epd_wait = 0u;
                /* BUSY never moved at all: the panel is absent, unpowered or
                 * wedged, so stop trusting "panel ready" and let the poller
                 * re-init it instead of writing into a dead framebuffer */
                s_panel_ready = 0u;
                panel_retry   = PANEL_RETRY_SLOTS;  /* re-init on the next pass */
                lg_s("no BUSY - panel re-init queued\r\n");
                if (s_img_hold != 0u) {
                    s_img_hold = 0u;
                    EPDTask_setImageTransfer(0);
                }
            }
        }

        /* release the clock hold if the phone delivered packets but never
         * sent the redraw (aborted session) */
        if (s_img_hold != 0u && g_epd_busy == 0u) {
            if ((s_img_idle += slots) > IMG_HOLD_MAX) {
                s_img_hold = 0u;
                EPDTask_setImageTransfer(0);
                lg_s("image hold released (no F0 D4)\r\n");
            }
        }

        /* never block on the UART while frames may still arrive: drain at
         * most LG_DRAIN characters, and only once the link went quiet */
        if (g_lg_n != 0u && idle > LOOP_IDLE_FLUSH) {
            lg_drain();
        }

        /* the exchange flag is for an external observer; let it decay when
         * the field goes quiet so it keeps meaning "something is happening" */
        if (idle > 5000u) g_nfc_busy = 0u;

        /* Sleep until an IRQ_N edge (the chip has something).  With
         * NFC_FALLBACK_MS == 0 there is no timeout at all - we block in
         * Event_pend and only the interrupt wakes us (lowest idle power).
         * A non-zero fallback also wakes periodically as a safety net. */
        if (hNfcEvent != NULL) {
            uint32_t to = (NFC_FALLBACK_MS == 0u)
                        ? (uint32_t)BIOS_WAIT_FOREVER
                        : (NFC_FALLBACK_MS * 1000u) / Clock_tickPeriod;
            uint32_t ev = Event_pend(hNfcEvent, Event_Id_NONE, NFC_EVENT_IRQ, to);
            if ((ev & NFC_EVENT_IRQ) != 0u) {
                if (++s_irq_wakes <= 8u) {
                    HWUART_Printf("IRQ_N wake #%u\r\n", (unsigned)s_irq_wakes);
                }
            }
        } else {
            /* no event object: keep a plain poll going */
            Task_sleep((NFC_FALLBACK_MS ? NFC_FALLBACK_MS : 20u) * 1000u
                       / Clock_tickPeriod);
        }
    }
}

void NfcEsl_createTask(void)
{
    Task_Params taskParams;

    Task_Params_init(&taskParams);
    taskParams.stack     = NfcTaskStack;
    taskParams.stackSize = NFC_TASK_STACK_SIZE;
    taskParams.priority  = NFC_TASK_PRIORITY;

    Task_construct(&NfcTask, NfcEsl_taskFxn, &taskParams, NULL);
}

uint8_t NfcEsl_busy(void)
{
    return g_nfc_busy;
}

/* Drop every flag that can keep the clock tick suppressed and hand the
 * panel back.  Called when the user explicitly asks for clock mode: a tap
 * that ended early (packets delivered but no F0 D4) can leave s_img_hold
 * set, and that alone is enough to freeze the calendar on screen forever.
 * g_epd_busy is deliberately left alone - it mirrors the BUSY pin and the
 * F0 DE answer must stay honest. */
void NfcEsl_yieldPanel(void)
{
    s_img_hold = 0u;
    g_nfc_busy = 0u;
    EPDTask_setImageTransfer(0);
}
