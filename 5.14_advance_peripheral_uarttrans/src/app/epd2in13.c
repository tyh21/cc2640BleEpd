
#include "epd2in13.h"


#include <ti/drivers/SPI.h>
#include <ti/drivers/spi/SPICC26XXDMA.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/BIOS.h>

#include "board.h"

 // for debug
//#include "inc/sdi_task.h"
#include "util.h"
 
#include <ti/drivers/PIN.h>


#define BLUE_LED_PIN             IOID_0     // low active

// Forward declarations
void EPD_2IN13_SendCommand(uint8_t Reg);
void EPD_2IN13_SendData(uint8_t Data);
void EPD_2IN13_ReadBusy(void);

/* IOID_13 used to be described here as a reed switch, but on this board it
 * carries the FM11NC08S IRQ_N input; the responder in nfc_esl.c owns it.  The
 * old REED_PIN macro was never referenced anywhere, so it is gone. */
#define TEST_PIN    IOID_15     // on back of pcb with 'test'

#define EPD_POWER_PIN             IOID_20       // low active

#define EPD_RST_PIN             IOID_10
#define EPD_DC_PIN              IOID_11
#define EPD_BUSY_PIN            IOID_9
#define EPD_CS_PIN              IOID_12



/*********************************************************************
 * LOCAL PARAMETER
 */   
static PIN_Handle GPIOHandle = NULL;
static PIN_State GPIOState;
static PIN_Config GPIOTable[] =
{
  EPD_BUSY_PIN          | PIN_GPIO_OUTPUT_DIS  | PIN_INPUT_EN  |  PIN_PULLUP,
  
  EPD_POWER_PIN | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MIN,
  
  EPD_DC_PIN | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MIN,
  EPD_RST_PIN | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MIN,
  EPD_CS_PIN | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MIN,

  // LED pins (low active)
  IOID_0  | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX,  // 蓝
  IOID_1  | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX,  // 红
  IOID_14 | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX,  // 绿

  PIN_TERMINATE
};

static SPI_Handle      SPIHandle = NULL;
static SPI_Params      SPIparams;


const unsigned char EPD_2IN13_lut_full_update[]= {
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,             //LUT0: BB:     VS 0 ~7
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,             //LUT1: BW:     VS 0 ~7
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,             //LUT2: WB:     VS 0 ~7
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,             //LUT3: WW:     VS 0 ~7
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,             //LUT4: VCOM:   VS 0 ~7

    0x03,0x03,0x00,0x00,0x02,                       // TP0 A~D RP0
    0x09,0x09,0x00,0x00,0x02,                       // TP1 A~D RP1
    0x03,0x03,0x00,0x00,0x02,                       // TP2 A~D RP2
    0x00,0x00,0x00,0x00,0x00,                       // TP3 A~D RP3
    0x00,0x00,0x00,0x00,0x00,                       // TP4 A~D RP4
    0x00,0x00,0x00,0x00,0x00,                       // TP5 A~D RP5
    0x00,0x00,0x00,0x00,0x00,                       // TP6 A~D RP6

    0x15,0x41,0xA8,0x32,0x30,0x0A,
};

// Partial refresh LUT for SSD1680 2.13" (no flicker, ~0.3-1s)
// Uses only VS0 (BB/WB/WB LUTs = clear to new value), no VCOM LUT
const unsigned char EPD_2IN13_lut_partial_update[]= {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,             //LUT0: BB
    0x80,0x00,0x00,0x00,0x00,0x00,0x00,             //LUT1: BW
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,             //LUT2: WB
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,             //LUT3: WW
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,             //LUT4: VCOM

    0x0A,0x00,0x00,0x00,0x00,                       // TP0
    0x00,0x00,0x00,0x00,0x00,                       // TP1
    0x00,0x00,0x00,0x00,0x00,                       // TP2
    0x00,0x00,0x00,0x00,0x00,                       // TP3
    0x00,0x00,0x00,0x00,0x00,                       // TP4
    0x00,0x00,0x00,0x00,0x00,                       // TP5
    0x00,0x00,0x00,0x00,0x00,                       // TP6

    0x15,0x41,0xA8,0x32,0x30,0x0A,
};

// quick and dirty way
// Task_sleep defined in <ti/sysbios/knl/Task.h>
static void Util_delay_ms(uint16_t t)
{  
  Task_sleep( ((t) * 1000) / Clock_tickPeriod );
}

static void HwUARTPrintf(const char *str)
{
 // HWUART_Printf(str);
}

static void DEV_Digital_Write(uint32_t pin, uint8_t value)
{    
    PIN_setOutputValue(GPIOHandle, pin, value);    
}

static int DEV_Digital_Read(uint32_t pin)
{
    int ret;
  ret = PIN_getInputValue(pin);
  return ret;
}

static void DEV_Delay_ms(uint16_t t)
{
    Util_delay_ms(t);
}

static void DEV_SPI_WriteByte(uint8_t byte)
{
    uint8_t txbuf[2];
    uint8_t rxbuf[2];
    
    txbuf[0] = byte;
    
  SPI_Transaction spiTransaction;
  spiTransaction.arg = NULL;
  spiTransaction.count = 1;
  spiTransaction.txBuf = txbuf;
  spiTransaction.rxBuf = rxbuf;
    
    

  bool ok =  SPI_transfer(SPIHandle, &spiTransaction);
 
  if (!ok) {
    HwUARTPrintf("spi transf fail\r\n");
  }
  
  
}

/* ---- panel arbitration ----
 * One binary semaphore guards the panel (SPI0 + SSD1680 RAM).  Binary and not
 * a priority-inheriting gate on purpose: the only two contenders are the EPD
 * task (priority 2) and the NFC task (priority 1), so when the higher-priority
 * EPD task blocks on a lock the NFC task holds, the scheduler simply runs NFC
 * to completion - there is no third task in that priority band to invert
 * against.  See epd2in13.h for the contract. */
static Semaphore_Struct EPD_panelSemStruct;
static Semaphore_Handle EPD_panelSem = NULL;

static void epd_panel_arb_init(void)
{
    Semaphore_Params sp;
    if (EPD_panelSem != NULL) return;
    Semaphore_Params_init(&sp);
    sp.mode = Semaphore_Mode_BINARY;
    Semaphore_construct(&EPD_panelSemStruct, 1, &sp);
    EPD_panelSem = Semaphore_handle(&EPD_panelSemStruct);
}

void EPD_Panel_Acquire(void)
{
    if (EPD_panelSem == NULL) epd_panel_arb_init();
    Semaphore_pend(EPD_panelSem, BIOS_WAIT_FOREVER);
}

void EPD_Panel_Release(void)
{
    if (EPD_panelSem != NULL) Semaphore_post(EPD_panelSem);
}

// should be only called once!
void epd_hw_init()
{
    // Idempotent on purpose: both the EPD task and the NFC task come up
    // through this call, and whichever runs first must be able to set it up
    // without the second one opening the same pins and SPI a second time.
    if (GPIOHandle != NULL) {
        return;
    }

    epd_panel_arb_init();   /* construct the panel lock exactly once */

    HwUARTPrintf("setup epd gpio\r\n");
    GPIOHandle = PIN_open(&GPIOState, GPIOTable);

    HwUARTPrintf("setup epd spi\r\n");
    SPI_init();
    SPI_Params_init(&SPIparams);
    SPIparams.bitRate  = 1000000;                    //1MHz
    SPIparams.dataSize = 8;
    SPIparams.frameFormat = SPI_POL0_PHA0;
    SPIparams.mode = SPI_MASTER;
    SPIparams.transferCallbackFxn = NULL;
    SPIparams.transferMode = SPI_MODE_BLOCKING;
    SPIparams.transferTimeout = SPI_WAIT_FOREVER;

    SPIHandle = SPI_open(CC2640R2_LAUNCHXL_SPI0, &SPIparams);
    if (NULL == SPIHandle) {
        HwUARTPrintf("spi open fail\r\n");
    }
}



/******************************************************************************
function :	Software reset
parameter:
******************************************************************************/
static void EPD_2IN13_Reset(void)
{
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(20);         /* 2 ms was marginal - a panel left latched
                               * by a previous firmware needs a solid low
                               * pulse to release it                        */
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(20);
}

/******************************************************************************
function :	send command
parameter:
     Reg : Command register
******************************************************************************/
void EPD_2IN13_SendCommand(uint8_t Reg)
{
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Reg);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :	send data
parameter:
    Data : Write data
******************************************************************************/
void EPD_2IN13_SendData(uint8_t Data)
{
    DEV_Digital_Write(EPD_DC_PIN, 1);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Data);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :	Wait until the busy_pin goes LOW

             The wait budget is a variable, and that matters a lot:
             - a full 2.13" update legitimately holds BUSY high for a few
               seconds, so the refresh path gets 15 s;
             - panel bring-up gets only 2 s, because a latched-busy panel
               used to burn the old fixed 30 s timeout three times in a row
               (EPD_2IN13_Init called ReadBusy 3x) and kept the whole NFC
               responder - and the boot banner - off the air for 91 seconds.
******************************************************************************/
static uint16_t s_busy_budget_ms = 15000u;
static uint16_t s_busy_stalls    = 0u;

/* How many times a BUSY wait ran out of budget since boot.  Exposed so the
 * NFC task can report it - 0 is the healthy case. */
uint16_t EPD_2IN13_BusyStalls(void)
{
    return s_busy_stalls;
}

void EPD_2IN13_ReadBusy(void)
{
    uint16_t waited = 0;

    while (DEV_Digital_Read(EPD_BUSY_PIN) == 1) {     //LOW: idle, HIGH: busy
        DEV_Delay_ms(50);
        waited = (uint16_t)(waited + 50u);
        if (waited >= s_busy_budget_ms) {
            s_busy_stalls++;
            break;
        }
    }
}


/******************************************************************************
function :	Load the full-refresh waveform set + the analog registers into the
            controller.

            EPD_2IN13_Init() sends exactly this as part of its 500 ms bring-up.
            The NFC image path cannot afford a bring-up - the reader's frame
            waiting time is 9.7 ms - but it DOES have to undo whatever the
            clock's partial render left behind: EPD_2IN13_SwitchToPartialLUT()
            switches VCOM and loads the partial waveform, and refreshing a
            whole new image with the partial waveform looks washed out.

            That is ~80 SPI bytes, well under a millisecond, so it fits inside
            an ISO-DEP exchange.  Byte sequence is identical to Init's.
******************************************************************************/
void EPD_2IN13_LoadFullLUT(void)
{
    uint8_t i;

    EPD_2IN13_SendCommand(0x03);                       // VCOM register
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[70]);

    EPD_2IN13_SendCommand(0x04);                       // VGH/VGL/VDL/VDH
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[71]);
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[72]);
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[73]);

    EPD_2IN13_SendCommand(0x3A);                       // dummy line
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[74]);
    EPD_2IN13_SendCommand(0x3B);                       // gate line width
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[75]);

    EPD_2IN13_SendCommand(0x32);                       // LUT register (70 B)
    for (i = 0; i < 70; i++) {
        EPD_2IN13_SendData(EPD_2IN13_lut_full_update[i]);
    }
}

/******************************************************************************
function :	Power the analog block down, but KEEP VCC and the RAM contents.

            This exists because of a confirmed field failure: the clock task
            ends every render with EPD_2IN13_Sleep(), which puts the panel in
            DEEP SLEEP and then cuts the PWR pin.  Coming back from that needs
            a hardware reset + full re-init (~500 ms), and 500 ms cannot happen
            inside an ISO-DEP exchange, so the NFC image path could never wake
            the panel: F0 D2 wrote into a dead framebuffer, F0 D4 refreshed
            nothing, and BUSY floated HIGH - which made F0 DE answer "busy"
            forever while the phone polled it.

            With this lighter power-off the panel is still resumable: a later
            "0x22 0xC7 / 0x20" re-enables the analog and refreshes from the
            RAM it already holds.  Idle current is the driver's standby, and
            that is the price of being tappable at any moment.
******************************************************************************/
void EPD_2IN13_SleepLight(void)
{
    EPD_2IN13_SendCommand(0x22);
    EPD_2IN13_SendData(0xC0);      // disable clock signal + analog
    EPD_2IN13_SendCommand(0x20);

    // deliberately NO 0x10 0x01 (deep sleep) and NO PWR pin change
}

/******************************************************************************
function :	Initialize the e-Paper register
parameter:
******************************************************************************/
void EPD_2IN13_Init()
{
    uint16_t saved_budget = s_busy_budget_ms;

    /* bring-up must never block for tens of seconds - see ReadBusy above */
    s_busy_budget_ms = 2000u;

    // power on (LOW active)
    DEV_Digital_Write(EPD_POWER_PIN, 0);
    DEV_Delay_ms(100);

    EPD_2IN13_Reset();

    /* Soft reset FIRST, then wait.  The old order (wait - reset - wait)
     * meant a panel still latched-busy from a previous firmware made the
     * very first wait run its whole timeout before 0x12 was ever sent. */
    EPD_2IN13_SendCommand(0x12); // soft reset
    EPD_2IN13_ReadBusy();

    EPD_2IN13_SendCommand(0x74); // set analog block control
    EPD_2IN13_SendData(0x54);
    EPD_2IN13_SendCommand(0x7E); // set digital block control
    EPD_2IN13_SendData(0x3B);

    // Driver output control: 250 scan lines (0xF9)
    // SSD1680 has 250 gate lines; visible 212 are a subset
    EPD_2IN13_SendCommand(0x01);
    EPD_2IN13_SendData(0xF9);  // 250-1 = 0xF9
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);

    EPD_2IN13_SendCommand(0x11); // data entry mode: Y increment, X increment
    EPD_2IN13_SendData(0x03);

    // RAM X: 0 ~ 0x0F (16 bytes = 128 pixels per row)
    EPD_2IN13_SendCommand(0x44);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x0F);

    // RAM Y: 0x00 ~ 0xF9 (250 lines)
    EPD_2IN13_SendCommand(0x45);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0xF9);
    EPD_2IN13_SendData(0x00);

    EPD_2IN13_SendCommand(0x3C); // border waveform
    EPD_2IN13_SendData(0x03);

    EPD_2IN13_SendCommand(0x2C); // VCI voltage
    EPD_2IN13_SendData(0x55);

    // Write VCOM, VGH/VGL/VDL/VDH, dummy/gate timing and the 70-byte full LUT.
    // Same sequence the NFC path replays on its own (EPD_2IN13_LoadFullLUT).
    EPD_2IN13_LoadFullLUT();

    // Set RAM X address counter
    EPD_2IN13_SendCommand(0x4E);
    EPD_2IN13_SendData(0x00);
    // Set RAM Y address counter
    EPD_2IN13_SendCommand(0x4F);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);

    EPD_2IN13_ReadBusy();

    s_busy_budget_ms = saved_budget;

    HwUARTPrintf("epd initialized\r\n");
}

/******************************************************************************
function :	Clear screen
parameter:
******************************************************************************/
void EPD_2IN13_Clear(void)
{
  HwUARTPrintf("epd clear\r\n");

    // Set RAM X counter to 0
    EPD_2IN13_SendCommand(0x4E);
    EPD_2IN13_SendData(0x00);
    // Set RAM Y counter to 0
    EPD_2IN13_SendCommand(0x4F);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);

    // Write 250 lines × 16 bytes = 4000 bytes of 0xFF (white)
    EPD_2IN13_SendCommand(0x24);
    for (uint16_t row = 0; row < 250; row++) {
        for (uint16_t col = 0; col < 16; col++) {
            EPD_2IN13_SendData(0xFF);
        }
    }

    EPD_2IN13_UpdateDisplay();
}

/******************************************************************************
function :	Sends the image buffer in RAM to e-Paper and displays
parameter:
******************************************************************************/
void EPD_2IN13_Display(const uint8_t *Image)
{
    // SSD1680 RAM layout: 250 scan lines × 16 bytes/row (128 pixels/row)
    // Visible area: 212 lines × 13 bytes (104 pixels)
    // Image is expected as 212 × 13 = 2756 bytes (row-major, 212 rows of 13 bytes each)

    // Set RAM X counter to 0
    EPD_2IN13_SendCommand(0x4E);
    EPD_2IN13_SendData(0x00);
    // Set RAM Y counter to 0
    EPD_2IN13_SendCommand(0x4F);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);

    EPD_2IN13_SendCommand(0x24);
    for (uint16_t row = 0; row < 250; row++) {
        for (uint16_t col = 0; col < 16; col++) {
            if (row < 212 && col < 13) {
                // Visible area: use image data
                EPD_2IN13_SendData(Image[col + row * 13]);
            } else {
                // Outside visible area: fill white
                EPD_2IN13_SendData(0xFF);
            }
        }
    }
    EPD_2IN13_UpdateDisplay();
}

void EPD_2IN13_PrepareBlkRAM(void)
{
  uint16_t row, col;
  // 先填满全白, 确保不可见区域和每行尾部3字节都是白色
  EPD_2IN13_SendCommand(0x4E);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendCommand(0x4F);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendCommand(0x24);
  for (row = 0; row < 250; row++) {
      for (col = 0; col < 16; col++) {
          EPD_2IN13_SendData(0xFF);
      }
  }

  // 重置地址到0, 准备接收可见区域数据 (212行×13字节)
  EPD_2IN13_SendCommand(0x4E);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendCommand(0x4F);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendCommand(0x24);
}

void EPD_2IN13_PrepareRedRAM(void)
{
  uint16_t row, col;
  // 先填满全白
  EPD_2IN13_SendCommand(0x4E);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendCommand(0x4F);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendCommand(0x26);
  for (row = 0; row < 250; row++) {
      for (col = 0; col < 16; col++) {
          EPD_2IN13_SendData(0xFF);
      }
  }

  // 重置地址到0, 准备接收可见区域数据 (212行×13字节)
  EPD_2IN13_SendCommand(0x4E);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendCommand(0x4F);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendData(0x00);
  EPD_2IN13_SendCommand(0x26);
}

void EPD_2IN13_WriteRAM(const uint8_t *buf, const int len)
{
    for (int i = 0; i < len; i++) {
        EPD_2IN13_SendData(buf[i]);
    }
}

/******************************************************************************
function :	Write a run of bytes at an explicit offset in the 4000-byte
             B/W RAM window (used by the NFC streaming path).

             pos = P2 * 250 + offset-inside-packet  (0..3999)
             X   = pos % 16  -> RAM X counter (0x4E)
             Y   = pos / 16  -> RAM Y counter (0x4F, 9 bits)

             The counter is re-armed on EVERY call, which is exactly what
             makes a retransmitted ISO-DEP block harmless: it lands on the
             cells it already wrote instead of running on.
******************************************************************************/
void EPD_2IN13_WriteRAMAt(uint32_t pos, const uint8_t *buf, int len)
{
    uint32_t y = pos >> 4;

    EPD_2IN13_SendCommand(0x4E);                       // RAM X address
    EPD_2IN13_SendData((uint8_t)(pos & 0x0Fu));

    EPD_2IN13_SendCommand(0x4F);                       // RAM Y address
    EPD_2IN13_SendData((uint8_t)(y & 0xFFu));
    EPD_2IN13_SendData((uint8_t)((y >> 8) & 0x01u));

    EPD_2IN13_SendCommand(0x24);                       // write to B/W RAM
    for (int i = 0; i < len; i++) {
        EPD_2IN13_SendData(buf[i]);
    }
}

/******************************************************************************
function :	Non-blocking BUSY probe (1 = busy, 0 = idle)
******************************************************************************/
uint8_t EPD_2IN13_IsBusy(void)
{
    return (uint8_t)(DEV_Digital_Read(EPD_BUSY_PIN) & 1);
}

/******************************************************************************
function :	Start a full refresh and return immediately.

             0x22 0xC7 = activate update with the full waveform, 0x20 = master
             activation.  These are three short SPI bytes; the seconds-long
             panel work happens afterwards, and the caller polls
             EPD_2IN13_IsBusy() instead of blocking a real-time task.

             The full LUT is re-loaded first on purpose: this entry point is
             used by the NFC image path, and the clock's partial render may
             have left the partial waveform (and VCOM) loaded.  ~80 bytes of
             SPI, well inside a frame waiting time, and it makes the result
             independent of what ran before.
******************************************************************************/
void EPD_2IN13_UpdateDisplayAsync(void)
{
    EPD_2IN13_LoadFullLUT();

    EPD_2IN13_SendCommand(0x22);
    EPD_2IN13_SendData(0xC7);
    EPD_2IN13_SendCommand(0x20);
}

void EPD_2IN13_UpdateDisplay(void)
{
     HwUARTPrintf("turn on display\r\n");
   
    EPD_2IN13_SendCommand(0x22);
    EPD_2IN13_SendData(0xC7);
    EPD_2IN13_SendCommand(0x20);
    EPD_2IN13_ReadBusy();
    DEV_Delay_ms(200);
}

/******************************************************************************
function: Switch LUT and VCOM to partial refresh mode (call after Init, before WriteRAM)
******************************************************************************/
/*
 * Switch to partial refresh mode after EPD_2IN13_Init().
 * Sets VCOM=0x28, partial LUT, 0x37 register, and activates partial mode.
 * Must be called after full Init (which sets up Driver Output, Data Entry, RAM window).
 */
void EPD_2IN13_SwitchToPartialLUT(void)
{
    uint8_t i;

    // Set lower VCOM for partial refresh (reduces ghosting)
    EPD_2IN13_SendCommand(0x2C);
    EPD_2IN13_SendData(0x28);

    EPD_2IN13_ReadBusy();

    // Write partial LUT (70 bytes)
    EPD_2IN13_SendCommand(0x32);
    for (i = 0; i < 70; i++) {
        EPD_2IN13_SendData(EPD_2IN13_lut_partial_update[i]);
    }

    // Write 0x37 register — partial update control
    EPD_2IN13_SendCommand(0x37);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x40);  // enable partial update
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);

    // Activate partial mode
    EPD_2IN13_SendCommand(0x22);
    EPD_2IN13_SendData(0xC0);
    EPD_2IN13_SendCommand(0x20);
    EPD_2IN13_ReadBusy();

    HwUARTPrintf("switched to partial LUT\r\n");
}

/******************************************************************************
function: Initialize for partial refresh (Init + load partial LUT, no sleep after update)
******************************************************************************/
/******************************************************************************
function: Initialize for partial refresh
 - Full Init (sets up Driver Output, Data Entry Mode, RAM window)
 - Then switch to partial LUT/VCOM/0x37
******************************************************************************/
void EPD_2IN13_InitPartial()
{
    EPD_2IN13_Init();
    EPD_2IN13_SwitchToPartialLUT();
}

/******************************************************************************
function: Partial refresh — display current RAM with no flicker, fast (~0.3-1s)
******************************************************************************/
/******************************************************************************
function: Partial refresh — activate display update with no flicker
 0x22 = 0x04: only activate display, no clock/LUT reload (already done in InitPartial)
******************************************************************************/
void EPD_2IN13_UpdatePartial(void)
{
    HwUARTPrintf("partial update\r\n");

    EPD_2IN13_SendCommand(0x22);
    EPD_2IN13_SendData(0x04);  // Partial: only activate display
    EPD_2IN13_SendCommand(0x20);
    EPD_2IN13_ReadBusy();
    DEV_Delay_ms(100);
}

/******************************************************************************
function: Display base image for partial mode (first time full refresh)
 - Writes same image to BOTH 0x24 (new) and 0x26 (old) RAM
 - SSD1680 compares them, finds no difference, establishes base image
 - After this, partial refresh can work correctly
parameter:
    Image : 212 rows x 13 bytes = 2756 bytes (row-major, landscape)
******************************************************************************/
void EPD_2IN13_DisplayPartBaseImage(const uint8_t *Image)
{
    uint16_t row, col;

    // Set RAM address to 0
    EPD_2IN13_SendCommand(0x4E);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendCommand(0x4F);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);

    // Write new image to 0x24 (black/white RAM)
    EPD_2IN13_SendCommand(0x24);
    for (row = 0; row < 250; row++) {
        for (col = 0; col < 16; col++) {
            if (row < 212 && col < 13) {
                EPD_2IN13_SendData(Image[col + row * 13]);
            } else {
                EPD_2IN13_SendData(0xFF);
            }
        }
    }

    // Write same image to 0x26 (old image RAM) — establishes base
    EPD_2IN13_SendCommand(0x4E);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendCommand(0x4F);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendCommand(0x26);
    for (row = 0; row < 250; row++) {
        for (col = 0; col < 16; col++) {
            if (row < 212 && col < 13) {
                EPD_2IN13_SendData(Image[col + row * 13]);
            } else {
                EPD_2IN13_SendData(0xFF);
            }
        }
    }
}

/******************************************************************************
function: Display partial update image (subsequent refreshes)
 - Writes new image to 0x24 (new RAM)
 - Writes INVERTED new image to 0x26 (old RAM)
 - SSD1680 internally XORs 0x24 and 0x26 to find changed pixels,
   then only drives those pixels — no flicker, fast
parameter:
    Image : 212 rows x 13 bytes = 2756 bytes (row-major, landscape)
******************************************************************************/
void EPD_2IN13_DisplayPart(const uint8_t *Image)
{
    uint16_t row, col;
    uint8_t b;

    // Set RAM address to 0
    EPD_2IN13_SendCommand(0x4E);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendCommand(0x4F);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);

    // Write new image to 0x24
    EPD_2IN13_SendCommand(0x24);
    for (row = 0; row < 250; row++) {
        for (col = 0; col < 16; col++) {
            if (row < 212 && col < 13) {
                EPD_2IN13_SendData(Image[col + row * 13]);
            } else {
                EPD_2IN13_SendData(0xFF);
            }
        }
    }

    // Write INVERTED new image to 0x26 (old image RAM)
    // SSD1680 does: pixel_changed = (new != old), drive only changed pixels
    EPD_2IN13_SendCommand(0x4E);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendCommand(0x4F);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendCommand(0x26);
    for (row = 0; row < 250; row++) {
        for (col = 0; col < 16; col++) {
            if (row < 212 && col < 13) {
                b = Image[col + row * 13];
                EPD_2IN13_SendData(~b);
            } else {
                EPD_2IN13_SendData(0x00);  // inverted 0xFF
            }
        }
    }
}

/******************************************************************************
function: Power off (no deep sleep) — for partial mode, allows quick re-init
******************************************************************************/
void EPD_2IN13_PowerOff(void)
{
    EPD_2IN13_SendCommand(0x22);  // Power off
    EPD_2IN13_SendData(0xC0);    // Disable clock, disable analog
    EPD_2IN13_SendCommand(0x20);
    EPD_2IN13_ReadBusy();

    DEV_Digital_Write(EPD_POWER_PIN, 1);  // Cut power
    DEV_Delay_ms(100);

    HwUARTPrintf("epd powered off\r\n");
}

/******************************************************************************
function :	Enter sleep mode
parameter:
******************************************************************************/
void EPD_2IN13_Sleep(void)
{
    EPD_2IN13_SendCommand(0x22); //POWER OFF
    EPD_2IN13_SendData(0xC3);
    EPD_2IN13_SendCommand(0x20);

    EPD_2IN13_SendCommand(0x10); //enter deep sleep
    EPD_2IN13_SendData(0x01);
    DEV_Delay_ms(100);
    
    // power off
  DEV_Digital_Write(EPD_POWER_PIN, 1);
  DEV_Delay_ms(100);
}

void EPD_2IN13_QuickCmd(uint8_t cmd)
{
    uint16_t row, col;
    switch (cmd) {
    case 0x30: // clear
        EPD_2IN13_Init();
        EPD_2IN13_Clear();
        EPD_2IN13_SleepLight();
        break;

    case 0x31: // checkerboard
        EPD_2IN13_Init();
        EPD_2IN13_SendCommand(0x4E);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x4F);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x24);
        for (row = 0; row < 250; row++) {
            for (col = 0; col < 16; col++) {
                if (row < 212 && col < 13) {
                    EPD_2IN13_SendData((((row / 8) + col) % 2 == 0) ? 0xFF : 0x00);
                } else {
                    EPD_2IN13_SendData(0xFF);
                }
            }
        }
        EPD_2IN13_UpdateDisplay();
        EPD_2IN13_SleepLight();
        break;

    case 0x32: // all black
        EPD_2IN13_Init();
        EPD_2IN13_SendCommand(0x4E);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x4F);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendData(0x00);
        EPD_2IN13_SendCommand(0x24);
        for (row = 0; row < 250; row++) {
            for (col = 0; col < 16; col++) {
                EPD_2IN13_SendData(0x00);
            }
        }
        EPD_2IN13_UpdateDisplay();
        EPD_2IN13_SleepLight();
        break;

    default:
        break;
    }
}

/******************************************************************************
LED 控制: IOID_0=蓝, IOID_1=红, IOID_14=绿, 低电平点亮
******************************************************************************/
static uint8_t ledState = 0;  // 0=全灭, 1=全亮

void EPD_2IN13_LedTest(void)
{
    // 蓝2秒
    DEV_Digital_Write(IOID_0, 0);   // 蓝亮
    DEV_Delay_ms(2000);
    DEV_Digital_Write(IOID_0, 1);   // 蓝灭

    // 红2秒
    DEV_Digital_Write(IOID_1, 0);   // 红亮
    DEV_Delay_ms(2000);
    DEV_Digital_Write(IOID_1, 1);   // 红灭

    // 绿2秒
    DEV_Digital_Write(IOID_14, 0);  // 绿亮
    DEV_Delay_ms(2000);
    DEV_Digital_Write(IOID_14, 1);  // 绿灭

    // 黄(红+绿)2秒
    DEV_Digital_Write(IOID_1, 0);
    DEV_Digital_Write(IOID_14, 0);
    DEV_Delay_ms(2000);
    DEV_Digital_Write(IOID_1, 1);
    DEV_Digital_Write(IOID_14, 1);

    // 青(绿+蓝)2秒
    DEV_Digital_Write(IOID_14, 0);
    DEV_Digital_Write(IOID_0, 0);
    DEV_Delay_ms(2000);
    DEV_Digital_Write(IOID_14, 1);
    DEV_Digital_Write(IOID_0, 1);

    // 紫(红+蓝)2秒
    DEV_Digital_Write(IOID_1, 0);
    DEV_Digital_Write(IOID_0, 0);
    DEV_Delay_ms(2000);
    DEV_Digital_Write(IOID_1, 1);
    DEV_Digital_Write(IOID_0, 1);

    // 白(红+绿+蓝)2秒
    DEV_Digital_Write(IOID_1, 0);
    DEV_Digital_Write(IOID_14, 0);
    DEV_Digital_Write(IOID_0, 0);
    DEV_Delay_ms(2000);
    DEV_Digital_Write(IOID_1, 1);
    DEV_Digital_Write(IOID_14, 1);
    DEV_Digital_Write(IOID_0, 1);
}

void EPD_2IN13_LedToggle(void)
{
    ledState = !ledState;
    DEV_Digital_Write(IOID_0,  ledState ? 0 : 1);
    DEV_Digital_Write(IOID_1,  ledState ? 0 : 1);
    DEV_Digital_Write(IOID_14, ledState ? 0 : 1);
}
