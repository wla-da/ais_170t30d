/*
компактный UART логгер для HC32L110
*/

#include "ulog.h"
#include "uart.h"


#define UART_BAUDRATE 115200

/*
    Инициализация UART1 на пинах P36 (Rx) и P35 (Tx)
    TODO пока только в режим Tx
*/
en_result_t init_ulog() {
    /**
     * Set PCLK = HCLK = Clock source to 24MHz
     */
    en_result_t res = Clk_Init(ClkFreq24Mhz, ClkDiv1, ClkDiv1);
    if (Ok != res) {
        return res;
    }
    // Enable peripheral clock
    CLK_EnablePeripheralClk(ClkPeripheralBaseTim);
    // GPIO clock is required
    CLK_EnablePeripheralClk(ClkPeripheralGpio);
    CLK_EnablePeripheralClk(ClkPeripheralUart1);
    /* 
     * Set P01,P02 as UART1 TX,RX, or use P35,P36
     */
    res = Gpio_InitIOExt(3,5,GpioDirOut,TRUE,FALSE,FALSE,FALSE);  
    if (Ok != res) {
        return res;
    } 
    res = Gpio_InitIOExt(3,6,GpioDirOut,TRUE,FALSE,FALSE,FALSE); 
    if (Ok != res) {
        return res;
    }
    Gpio_SetFunc_UART1_TX_P35();
    Gpio_SetFunc_UART1_RX_P36();
     
    /*
    Gpio_InitIOExt(0,1,GpioDirOut,TRUE,FALSE,FALSE,FALSE);
    Gpio_InitIOExt(0,2,GpioDirOut,TRUE,FALSE,FALSE,FALSE);
    Gpio_SetFunc_UART1_TXD_P01();
    Gpio_SetFunc_UART1_RXD_P02();
    */

    // Config UART1
    //Uart1_TxRx_Init(UART_BAUDRATE, APP_UART_RX_Callback)
    Uart1_Init(UART_BAUDRATE);
    return Ok; 
}

/*
    Вывод одного символа в лог.
*/
void log_write_char(char c)
{
    Uart1_TxChar(c);
}

/*
    Вывод строки.
    Строка должна быть null-terminated.
*/
void log_write_str(char *s)
{
    Uart1_TxString(s);
}

/*
    Вывод hex
*/
void log_write_hex(uint8_t h)
{
    Uart1_TxHex8Bit(h);
}

/*
    Вывод беззнакового 32-битного числа в десятичном виде.

    Используется простой алгоритм деления на 10.
    Буфер хранит цифры в обратном порядке.
*/
void log_write_u32(uint32_t value)
{
    char buffer[11];
    uint8_t index = 0;

    if (value == 0)
    {
        Uart1_TxChar('0');
        return;
    }

    while (value > 0)
    {
        uint32_t digit = value % 10;
        buffer[index] = (char)('0' + digit);
        index++;

        value = value / 10;
    }

    while (index > 0)
    {
        index--;
        Uart1_TxChar(buffer[index]);
    }
}

void log_write_i32(int32_t value) {
    uint32_t u_value;
    if (value < 0) {
        Uart1_TxChar('-');
        // 1. Приводим к 64 битам, чтобы +2147483648 влезло
        // 2. Инвертируем знак
        // 3. Сохраняем в uint32_t
        u_value = (uint32_t)(-(int64_t)value);
    } 
    else {
        u_value = (uint32_t)value;
    }
    
    log_write_u32(u_value);
}

/*
    Формирование строки лога.

    Формат:
    <LEVEL> <TAG> <VALUE>\n

    Пример:
    I ADC 123
*/
void log_write_line(char level, char *tag, uint32_t value)
{
    log_write_char(level);
    log_write_char(' ');

    log_write_str(tag);
    log_write_char(' ');

    log_write_u32(value);

    log_write_char('\r');
    log_write_char('\n');
}