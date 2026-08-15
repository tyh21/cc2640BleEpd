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

  };
 
 
 
#define EPD_TASK_PRIORITY                     2
#define EPD_TASK_STACK_SIZE                   2048
Task_Struct EPDTask;
Char EPDTaskStack[EPD_TASK_STACK_SIZE];

// cc2640 received epd command frame from andoid app
// first byte is length, second is command, third and follow are command data if any
static uint8_t epd_rx_frame[251];
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
      // Skip clock refresh while image transfer is in progress
      if (!s_img_transfer_active) {
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
    break;
    
    case EPD_CMD_DEINIT:
    HWUART_Printf("deinit\r\n");
    s_img_transfer_active = 0;  // image transfer done
     #ifndef EPD_DRY_RUN
      EPD_2IN13_Sleep();
#endif
    break;
    
    case EPD_CMD_CLEAR:
    HWUART_Printf("clear\r\n");
    s_img_transfer_active = 0;
    #ifndef EPD_DRY_RUN
    EPD_2IN13_QuickCmd(0x30);
#endif
    break;
    
    case EPD_CMD_TEST_PATTERN:
    HWUART_Printf("test pattern\r\n");
    s_img_transfer_active = 0;
    #ifndef EPD_DRY_RUN
    EPD_2IN13_QuickCmd(0x31);
#endif
    break;
    
    case EPD_CMD_ALL_BLACK:
    HWUART_Printf("all black\r\n");
    s_img_transfer_active = 0;
    #ifndef EPD_DRY_RUN
    EPD_2IN13_QuickCmd(0x32);
#endif
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

  default:
    HWUART_Printf("unknown cmd\r\n");
    resp_fram_len = 0;
    break;
  }

    
}

void EPDTask_parseCommand(uint8_t *pMsg, uint8_t length)
{
  memcpy(epd_rx_frame, pMsg, length);
  rx_fram_len = length;
    Event_post(hEPDEvent, EPDTASK_EVENT_RX_REQUEST);
  
}

static void clockTickCb(UArg arg)
{
  epd_clock_tick();
  Event_post(hEPDEvent, EPDTASK_EVENT_CLOCK_TICK);
}