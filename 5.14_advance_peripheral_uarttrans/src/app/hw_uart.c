#include "hw_uart.h"

#include "board.h"
#include <ti/drivers/uart/UARTCC26XX.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static UART_Handle UARTHandle;
static UART_Params UARTparams;

static void Uart_ReadCallback(UART_Handle handle, void *rxBuf, size_t size)
{ 
}

static void Uart_WriteCallback(UART_Handle handle, void *txBuf, size_t size)
{
  
}

void HWUART_Init()
{
  
  UART_init();                                      //��ʼ��ģ��Ĵ��ڹ���
  UART_Params_init(&UARTparams);                    //��ʼ�����ڲ���
  UARTparams.baudRate = 115200;                     //���ڲ�����115200
  UARTparams.dataLength = UART_LEN_8;               //��������λ8
  UARTparams.stopBits = UART_STOP_ONE;              //����ֹͣλ1
  UARTparams.readDataMode = UART_DATA_BINARY;       //���ڽ������ݲ�������
  UARTparams.writeDataMode = UART_DATA_TEXT;        //���ڷ������ݲ�������
  UARTparams.readMode = UART_MODE_CALLBACK;         //�����첽��
  UARTparams.writeMode = UART_MODE_BLOCKING;        //   
  UARTparams.readEcho = UART_ECHO_OFF;              //���ڲ�����
  UARTparams.readReturnMode = UART_RETURN_NEWLINE;  //�����յ����з�ʱ���ص�
  UARTparams.readCallback = Uart_ReadCallback;      //
  UARTparams.writeCallback = Uart_WriteCallback;    //
  
  UARTHandle = UART_open(Board_UART0, &UARTparams); //�򿪴���ͨ��
 // UART_control(UARTHandle, UARTCC26XX_RETURN_PARTIAL_ENABLE,  NULL);   //�������ղ��ֻص�
  
}

void HWUART_Printf(const char* format, ...)
{
      va_list arg;

  /* May be called from an assert raised before HWUART_Init(), or from a
   * context that never ran it at all - a NULL handle must be a no-op and
   * never a dereference. */
  if (UARTHandle == NULL) {
    return;
  }

  va_start(arg,format);
  uint8_t buf[108];
  uint16_t len;
  len = vsprintf((char*)buf, format, arg);
  va_end(arg);
  UART_write(UARTHandle, buf, len);
}

// Raw write with an explicit length - no formatting, no temporary buffer.
// The NFC log drain uses it so that "flush" has a predictable cost.
void HWUART_Write(const uint8_t *buf, uint16_t len)
{
  if (UARTHandle != NULL && len > 0) {
    UART_write(UARTHandle, (void *)buf, len);
  }
}