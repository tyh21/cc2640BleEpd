#include <stdio.h>
#include <string.h>

#include <xdc/std.h>
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Queue.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Event.h>

#include "board.h"
//#include <ti/drivers/uart/UARTCC26XX.h>
#include "task_epd.h"
#include "epd_clock.h"
#include "nfc_esl.h"     /* NfcEsl_busy(): don't start a render under a tap */


// if defined, not calling actual epd function, just test protocol
//#define EPD_DRY_RUN

#ifndef EPD_DRY_RUN
#include "epd2in13.h"
#endif
 
 // for debug
//#include "inc/sdi_task.h"

#include "hw_uart.h"

#include "util.h"

uint8_t VERSION_MAJOR = 0;
uint8_t VERSION_MINOR = 2;

 
 
 enum {
   EPD_CMD_PING = 0x10,
   EPD_CMD_INIT = 0x11,
   EPD_CMD_DEINIT = 0x12,
    
   EPD_CMD_PREPARE_BLK_RAM = 0x13,
   EPD_CMD_WRITE_BLK_RAM = 0x14,
   EPD_CMD_GET_BLK_RAM_CRC = 0x15,
   
   EPD_CMD_PREPARE_RED_RAM = 0x18,
   EPD_CMD_WRITE_RED_RAM = 0x19,
   EPD_CMD_GET_RED_RAM_CRC = 0x20,

   EPD_CMD_UPDATE_DISPLAY = 0x21,  // ask epd to show ram data
   
    EPD_CMD_READ_VERSION = 0x22,

    EPD_CMD_CLEAR = 0x30,       // 一键清屏: Init+Clear+Sleep
    EPD_CMD_TEST_PATTERN = 0x31, // 一键测试: Init+棋盘格+Sleep
    EPD_CMD_ALL_BLACK = 0x32,    // 一键全黑: Init+全黑+Sleep

    EPD_CMD_LED_TEST = 0x40,     // LED测试: 三色各亮2秒
    EPD_CMD_LED_TOGGLE = 0x41,   // LED开关: 三色全亮/全灭切换

    EPD_CMD_SET_TIME = 0x50,     // 设置时间
    EPD_CMD_GET_TIME = 0x51,     // 获取时间

    EPD_CMD_SWITCH_FACE = 0x60,  // 切换时钟表盘

    EPD_CMD_SET_MODE = 0x70,     // 显示模式: data[0] = 0 时钟 / 1 图片
    EPD_CMD_GET_MODE = 0x71,     // 读显示模式, resp[1] = 当前模式

  };
 
 
 
#define EPD_TASK_PRIORITY                     2
/* Stack budget, and it is NOT free: everything this task declares comes
 * out of the same ICall heap the BLE5 stack allocates from.  2048 was
 * pure waste, 1536 was still generous, and 1024 is ~2x the deepest real
 * path (Event_pend -> handle_cmd -> epd_clock_render -> HWUART_Printf's
 * own 108-byte format buffer).  The 512 bytes handed back here are what
 * lets the BLE stack finish a central's service discovery. */
#define EPD_TASK_STACK_SIZE                   1024
Task_Struct EPDTask;
Char EPDTaskStack[EPD_TASK_STACK_SIZE];

// cc2640 received epd command frame from andoid app
// first byte is length, second is command, third and follow are command data if any
// 160 is bigger than any frame the BLE path can deliver: MAX_PDU_SIZE=135
// caps one ATT write at 128 bytes of payload, so 251 was 91 wasted bytes of
// the ICall heap.  EPDTask_parseCommand drops anything larger instead of
// overrunning the buffer.
#define EPD_RX_FRAME_LEN   160
static uint8_t epd_rx_frame[EPD_RX_FRAME_LEN];
uint8_t rx_fram_len = 0;

static uint8_t epd_resp_frame[11];
uint8_t resp_fram_len = 1;

EpdResponseCallback respCallback = NULL;

// Event used to control the EPD thread
Event_Struct EPDEvent;
Event_Handle hEPDEvent;

#define EPDTASK_EVENT_RX_REQUEST      Event_Id_00
#define EPDTASK_EVENT_CLOCK_TICK      Event_Id_01


#define EPDTASK_EVENT_ALL ( EPDTASK_EVENT_RX_REQUEST | EPDTASK_EVENT_CLOCK_TICK )

// Flag: image transfer in progress, pause clock refresh
static uint8_t s_img_transfer_active = 0;
                            
                                         

static void handle_cmd();       
void TaskEPD_taskFxn(UArg a0, UArg a1);
static void post_epd_response(uint8_t *buf, uint16_t len);

// Clock tick period (ms)
#define CLOCK_TICK_PERIOD_MS  1000
static Clock_Struct clockTickClock;
static void clockTickCb(UArg arg);


void TaskEPD_createTask(void)
{
  Task_Params taskParams;

  // Configure task
  Task_Params_init(&taskParams);
  taskParams.stack = EPDTaskStack;
  taskParams.stackSize = EPD_TASK_STACK_SIZE;
  taskParams.priority = EPD_TASK_PRIORITY;

  Task_construct(&EPDTask, TaskEPD_taskFxn, &taskParams, NULL);
}

/*********************************************************************
 * @fn      TaskEPD_taskInit
 *
 * @brief   ���ڳ�ʼ��
 *
 * @param   None
 *
 * @return  None.
 */
void TaskEPD_taskInit(void)
{
    HWUART_Printf("TaskEPD_taskInit\r\n");
    
#ifndef EPD_DRY_RUN
     epd_hw_init();
#endif

    // Initialize clock subsystem
    epd_clock_init();

    // Start 1-second periodic clock for clock tick
    Util_constructClock(&clockTickClock, clockTickCb,
                         CLOCK_TICK_PERIOD_MS, CLOCK_TICK_PERIOD_MS, true, 0);
}

/*********************************************************************
 * @fn      TaskEPD_taskFxn
 *
 * @brief   ����������
 *
 * @param   None
 *
 * @return  None.
 */
void TaskEPD_taskFxn(UArg a0, UArg a1)
{ 
  Event_Params evParams;
  Event_Params_init(&evParams);
  Event_construct(&EPDEvent, &evParams);
  hEPDEvent = Event_handle(&EPDEvent);
  
  TaskEPD_taskInit();

  while(1)
  {
    UInt events;
    events = Event_pend(hEPDEvent,Event_Id_NONE, EPDTASK_EVENT_ALL, BIOS_WAIT_FOREVER);
    
    if(events & EPDTASK_EVENT_RX_REQUEST)
    {
      handle_cmd();
      
      
      // debug
   //   Util_delay_ms(10*1000);
      // send response
      if (resp_fram_len)
      {
        post_epd_response(epd_resp_frame, resp_fram_len);
      }
      
    }

    if(events & EPDTASK_EVENT_CLOCK_TICK)
    {
      // Skip clock refresh while an image transfer is in progress, and also
      // while a phone is on the NFC antenna: a render takes seconds and drives
      // the same SPI + the same panel RAM the NFC responder is streaming into,
      // so starting one under a tap would corrupt both.  (A render already in
      // flight when the tap begins still races - re-tap if the image looks
      // wrong.)
      if (!s_img_transfer_active && !NfcEsl_busy()) {
        uint8_t refresh = epd_clock_need_refresh();
        if (refresh)
        {
#ifndef EPD_DRY_RUN
          if (refresh == 2) {
            epd_clock_render(0);  // full refresh (after set_time)
          } else {
            epd_clock_render(1);  // partial refresh (minute boundary)
          }
#endif
        }
      }
    }
       
  }
}


void EPDTask_RegisterResponseCallback(EpdResponseCallback callback)
{
    respCallback = callback;
}

//void epd_on_rx_cmd(const uint8_t *buf, int len)
//{
//    memcpy(epd_rx_frame, buf, len);
//    Event_post(hEPDEvent, EPDTASK_EVENT_RX_REQUEST);
//}



static void post_epd_response(uint8_t *buf, uint16_t len)
{
    
    if (respCallback != NULL) {
        // 0x10 is the event id , should be the same in peripheral_uarttrans.c
        respCallback(0x10, buf, len);
    }

}

void handle_cmd()
{
  uint8_t cmd = epd_rx_frame[0];
  
  resp_fram_len  = 1;
  epd_resp_frame[0] = cmd;
  
  
  switch(cmd) {
    case EPD_CMD_READ_VERSION:
    HWUART_Printf("read ver\r\n");
    resp_fram_len  = 3;
    epd_resp_frame[1] = VERSION_MAJOR;
    epd_resp_frame[2] = VERSION_MINOR;
    
    break;
    
  case EPD_CMD_PING:
    HWUART_Printf("ping\r\n");
    break;
    
    case EPD_CMD_INIT:
    HWUART_Printf("init\r\n");
    
    #ifndef EPD_DRY_RUN
    EPD_2IN13_Init();
#endif
 
    break;
    
    case EPD_CMD_PREPARE_BLK_RAM:
    HWUART_Printf("prep blk ram\r\n");
    s_img_transfer_active = 1;
    #ifndef EPD_DRY_RUN
   EPD_2IN13_PrepareBlkRAM();
#endif
    break;
    
    case EPD_CMD_WRITE_BLK_RAM:
    HWUART_Printf("write blk ram\r\n");
 #ifndef EPD_DRY_RUN
   EPD_2IN13_WriteRAM(epd_rx_frame + 1, rx_fram_len - 1);
#endif
    break;
    
        case EPD_CMD_PREPARE_RED_RAM:
    HWUART_Printf("prep red ram\r\n");
    #ifndef EPD_DRY_RUN
   EPD_2IN13_PrepareRedRAM();
#endif
    break;
    
    case EPD_CMD_WRITE_RED_RAM:
    HWUART_Printf("write red ram\r\n");
 #ifndef EPD_DRY_RUN
   EPD_2IN13_WriteRAM(epd_rx_frame + 1, rx_fram_len - 1);
#endif
    break;
    
    
    
   case EPD_CMD_UPDATE_DISPLAY:
    HWUART_Printf("update display\r\n");
    s_img_transfer_active = 0;  // image transfer done
     #ifndef EPD_DRY_RUN
   EPD_2IN13_UpdateDisplay();
#endif
    /* The picture is on the panel now, so hand the screen over to image
     * mode: the 1-minute clock tick stops touching it and the image stays
     * up until the user switches back (0x70 mode=0).  Without this the
     * clock repaints itself over the image within a minute. */
    epd_clock_set_mode(EPD_MODE_IMAGE);
    HWUART_Printf("display mode -> image (sticky)\r\n");
    break;
    
    case EPD_CMD_DEINIT:
    HWUART_Printf("deinit\r\n");
    s_img_transfer_active = 0;  // image transfer done
     #ifndef EPD_DRY_RUN
      /* SleepLight, not Sleep: the panel must stay resumable for an NFC tap.
       * Deep sleep + a cut PWR pin can only be undone by a ~500 ms re-init,
       * which cannot happen inside an ISO-DEP exchange. */
      EPD_2IN13_SleepLight();
#endif
    break;
    
    case EPD_CMD_CLEAR:
    HWUART_Printf("clear\r\n");
    s_img_transfer_active = 0;
    #ifndef EPD_DRY_RUN
    EPD_2IN13_QuickCmd(0x30);
#endif
    /* Rule: whatever the app pushes onto the panel owns the panel until the
     * user switches back with 0x70 mode=0.  Without this the clock repaints
     * itself over the pushed content within a minute. */
    epd_clock_set_mode(EPD_MODE_IMAGE);
    break;
    
    case EPD_CMD_TEST_PATTERN:
    HWUART_Printf("test pattern\r\n");
    s_img_transfer_active = 0;
    #ifndef EPD_DRY_RUN
    EPD_2IN13_QuickCmd(0x31);
#endif
    epd_clock_set_mode(EPD_MODE_IMAGE);
    break;
    
    case EPD_CMD_ALL_BLACK:
    HWUART_Printf("all black\r\n");
    s_img_transfer_active = 0;
    #ifndef EPD_DRY_RUN
    EPD_2IN13_QuickCmd(0x32);
#endif
    epd_clock_set_mode(EPD_MODE_IMAGE);
    break;

    case EPD_CMD_LED_TEST:
    HWUART_Printf("led test\r\n");
    EPD_2IN13_LedTest();
    break;

    case EPD_CMD_LED_TOGGLE:
    HWUART_Printf("led toggle\r\n");
    EPD_2IN13_LedToggle();
    break;

    case EPD_CMD_SET_TIME:
    HWUART_Printf("set time\r\n");
    // data: year_lo, month, day, hour, minute, second, ms_hi, ms_lo (8 bytes)
    if (rx_fram_len >= 9) {
        epd_clock_set_time(epd_rx_frame + 1);
    }
    // resp_fram_len defaults to 1 (set at top of handle_cmd), echoes cmd byte as ACK
    break;

    case EPD_CMD_GET_TIME:
    HWUART_Printf("get time\r\n");
    {
        epd_time_t t = epd_clock_get_time();
        resp_fram_len = 9;
        epd_resp_frame[1] = (uint8_t)(t.year - 2000U);
        epd_resp_frame[2] = t.month;
        epd_resp_frame[3] = t.day;
        epd_resp_frame[4] = t.hour;
        epd_resp_frame[5] = t.minute;
        epd_resp_frame[6] = t.second;
        epd_resp_frame[7] = (uint8_t)(t.millisecond >> 8);
        epd_resp_frame[8] = (uint8_t)(t.millisecond & 0xFF);
    }
    break;

    case EPD_CMD_SWITCH_FACE:
    HWUART_Printf("switch face\r\n");
    epd_clock_switch_face();
    resp_fram_len = 2;
    epd_resp_frame[1] = epd_clock_get_face();
    break;

    case EPD_CMD_SET_MODE:
    // data[0] = 0 clock / 1 image.  Setting image mode is what keeps a
    // pushed picture on screen instead of being overwritten a minute later.
    if (rx_fram_len >= 2) {
        HWUART_Printf("set mode %u\r\n", (unsigned)epd_rx_frame[1]);
        if (epd_rx_frame[1] == 0) {
            /* Asking for the clock is an explicit "give me the screen back":
             * drop every hold first (NFC's image hold, the transfer flag, the
             * exchange flag) - a hold left behind by an aborted tap would
             * otherwise keep the tick suppressed and the calendar would never
             * return.  Then re-arm the full render and run it NOW instead of
             * waiting up to a second for the next periodic tick. */
            NfcEsl_yieldPanel();
            s_img_transfer_active = 0;
            epd_clock_set_mode(EPD_MODE_CLOCK);
            Event_post(hEPDEvent, EPDTASK_EVENT_CLOCK_TICK);
        } else {
            epd_clock_set_mode(EPD_MODE_IMAGE);
        }
    } else {
        HWUART_Printf("set mode (no arg)\r\n");
    }
    resp_fram_len = 2;
    epd_resp_frame[1] = epd_clock_get_mode();
    break;

    case EPD_CMD_GET_MODE:
    resp_fram_len = 2;
    epd_resp_frame[1] = epd_clock_get_mode();
    break;

  default:
    HWUART_Printf("unknown cmd\r\n");
    resp_fram_len = 0;
    break;
  }

    
}

void EPDTask_parseCommand(uint8_t *pMsg, uint8_t length)
{
  if (length > EPD_RX_FRAME_LEN) {
    HWUART_Printf("epd frame too long (%u) - dropped\r\n", (unsigned)length);
    return;
  }
  memcpy(epd_rx_frame, pMsg, length);
  rx_fram_len = length;
    Event_post(hEPDEvent, EPDTASK_EVENT_RX_REQUEST);
  
}

static void clockTickCb(UArg arg)
{
  epd_clock_tick();
  Event_post(hEPDEvent, EPDTASK_EVENT_CLOCK_TICK);
}

// Called by the NFC responder: while it is streaming an image into the
// panel, the 1-second clock refresh must not touch the same SPI/RAM.
void EPDTask_setImageTransfer(uint8_t active)
{
    s_img_transfer_active = active ? 1 : 0;
}