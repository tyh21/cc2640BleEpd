
#include "epd2in13.h"


#include <ti/drivers/SPI.h>
#include <ti/drivers/spi/SPICC26XXDMA.h>
#include <ti/sysbios/knl/Task.h>

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

#define REED_PIN    IOID_13     // reed switch, ground when there is magnet
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

// should be only called once!
void epd_hw_init()
{

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
    DEV_Delay_ms(200);
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(2);
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(200);
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
parameter:
******************************************************************************/
void EPD_2IN13_ReadBusy(void)
{
    HwUARTPrintf("e-Paper busy\r\n");
    uint16_t timeout = 0;
    while(DEV_Digital_Read(EPD_BUSY_PIN) == 1) {      //LOW: idle, HIGH: busy
        DEV_Delay_ms(100);
        timeout++;
        if (timeout > 300) {  // 30秒超时
            HwUARTPrintf("e-Paper busy timeout!\r\n");
            break;
        }
    }
    HwUARTPrintf("e-Paper busy release\r\n");
}


/******************************************************************************
function :	Initialize the e-Paper register
parameter:
******************************************************************************/
void EPD_2IN13_Init()
{
    uint8_t i;
    
    // power on (LOW active)
    DEV_Digital_Write(EPD_POWER_PIN, 0);
    DEV_Delay_ms(100);

    EPD_2IN13_Reset();

    EPD_2IN13_ReadBusy();
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

    // Write VCOM register
    EPD_2IN13_SendCommand(0x03);
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[70]);

    // Write VGH/VGL/VDL/VDH register
    EPD_2IN13_SendCommand(0x04);
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[71]);
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[72]);
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[73]);

    // Set dummy line and gate time
    EPD_2IN13_SendCommand(0x3A);
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[74]);
    EPD_2IN13_SendCommand(0x3B);
    EPD_2IN13_SendData(EPD_2IN13_lut_full_update[75]);

    // Write LUT register (70 bytes)
    EPD_2IN13_SendCommand(0x32);
    for (i = 0; i < 70; i++) {
        EPD_2IN13_SendData(EPD_2IN13_lut_full_update[i]);
    }

    // Set RAM X address counter
    EPD_2IN13_SendCommand(0x4E);
    EPD_2IN13_SendData(0x00);
    // Set RAM Y address counter
    EPD_2IN13_SendCommand(0x4F);
    EPD_2IN13_SendData(0x00);
    EPD_2IN13_SendData(0x00);

    EPD_2IN13_ReadBusy();

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
        EPD_2IN13_Sleep();
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
        EPD_2IN13_Sleep();
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
        EPD_2IN13_Sleep();
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
