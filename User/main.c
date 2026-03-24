/**
 * Управлением радиочипом SX1278 на плате E32 170T30D для приема AIS сигнала
 * 
 * Для отладки используется порт UART1 MCU Rx->M0, Tx->M1
 * 
 * Радиочип SX1278 подключен через SPI
 * 
 * 
 Распиновка
| MCU (pin)| функция SPI  | SX1278 (pin) |
| -------- | ------------ | ----------- |
| P1.4   | SPI CS (NSS) | NSS    |
| P2.3   | SPI MISO     | MISO   |
| P2.4   | SPI MOSI     | MOSI   |
| P1.5   | SPI SCK      | SCK    |

| MCU (pin)   | SX1278 (pin) |
| ------ | ------ |
| P3.2   | RESET  |

| Режим | P3.3 () | P3.4 () | Комментарий      |
| ----- | ------- | ------- | ---------------- |
| RX    | 0       | 1      | Пин P3.3 низкий/P3.4 высокий для приёма   |
| TX    | 1       | 0      | Пин P3.3 высокий/P3.4 низкий для передачи |
| Sleep | 0       | 0      | Оба пина низкие — модуль в сон            |

*/
#include "gpio.h"
#include "ulog.h"
#include "sx1278.h"

#define TAG "AIS"


/* Определяем пины управления питанием LNA и PA через константы */
#define ON_OFF_POWER_PAUSE_MS 20 //время на стабилизацию/переключение LDO
typedef struct {
    uint8_t port;
    uint8_t pin;
} gpio_pin_t;

static const gpio_pin_t P_TX_CTRL = {3, 3}; // P3.3
static const gpio_pin_t P_RX_CTRL  = {3, 4}; // P3.4

//флаг инициализации портов для управления питанием LNA/PA и тп
static boolean_t is_ports_init = FALSE;

/**
 * Выключает источники питания LNA и PA
 */
en_result_t power_tx_rx_off() {
    GPIO_SetPinOutLow(P_TX_CTRL.port, P_TX_CTRL.pin);
    GPIO_SetPinOutLow(P_RX_CTRL.port,  P_RX_CTRL.pin);
    delay1ms(ON_OFF_POWER_PAUSE_MS); // подождать стабилизации LDO

    return Ok;
}


/**
 * Инициализация пинов управления LNA/PA
 */
static void power_ctrl_init(void) {
    if (is_ports_init) {
        return;
    }
    Gpio_InitIOExt(P_TX_CTRL.port, P_TX_CTRL.pin, GpioDirOut, FALSE, FALSE, FALSE, FALSE);
    Gpio_InitIOExt(P_RX_CTRL.port, P_RX_CTRL.pin,  GpioDirOut, FALSE, FALSE, FALSE, FALSE);
    //переводим в Sleep явно
    power_tx_rx_off(); 
    
    is_ports_init = TRUE;
}


/**
 * Включает режим Rx, LNA для приема
 */
en_result_t power_rx_on() {
    // Переход в безопасный режим Sleep
    power_tx_rx_off();

    // RX: PA выключен, LNA включен
    GPIO_SetPinOutLow(P_TX_CTRL.port, P_TX_CTRL.pin); //перестраховка
    GPIO_SetPinOutHigh(P_RX_CTRL.port, P_RX_CTRL.pin);
    delay1ms(ON_OFF_POWER_PAUSE_MS); // подождать стабилизации LDO
    
    return Ok;
}


/**
 * Базовый сценарий инициализации приема
 */
en_result_t init() {
    //выключаем питание LNA/PA
    power_ctrl_init();
    power_tx_rx_off();

    return sx1278_init_rx_ais(rx_mode_packet, AIS_FREQ_LOWER_HZ);
}


int main(void) {
    power_ctrl_init();

    en_result_t res = init_ulog();
    LOGI(TAG, res);

    //узнаем частоту работы MCU
    LOGI(TAG, Clk_GetHClkFreq()/1000000);
    //узнаем частоту работы периферии для SPI, UART, Timer, I2C 
    LOGI(TAG, Clk_GetPClkFreq()/1000000);

    res = init();
    if (Ok != res) {
        LOGE(TAG, res);
        log_write_str("Un-init");
        return 1;
    }

    //включаем питание LNA и переключаем ВЧ-ключ на прием
    res = power_rx_on();
    LOGI(TAG, res);
    log_write_str("Rx on\r\n");
    
    uint8_t buff[FIXED_PACKET_LEN] = {0};
    uint16_t const buff_len = sizeof(buff);

    while(TRUE) {
        //читаем уровень сигнала
        uint8_t rssi = sx1278_get_rssi();
        LOGI(TAG, rssi);

        uint16_t read_bytes = 0;
        res = sx1278_read_packet(buff, buff_len, &read_bytes);
        LOGI(TAG, res);

        while (Ok != res) {
            LOGE(TAG, res);
            //обнуляем кол-во прочитанных байт
            read_bytes = 0;
            //явно сбрасываем радиочип
            sx1278_reset();
            //пробуем еще раз инициализировать
            res = sx1278_init_rx_ais(rx_mode_packet, AIS_FREQ_LOWER_HZ);
            if (Ok != res) {
                LOGE(TAG, res);
                delay1ms(5*1000);
            }
            else {
                //вернемся к попытке чтения принятых данных из радиочипа
                break;
            }
        }
        //передаем/обрабатываем принятые байты
        //TODO сколько времени это занимает, не успеет ли переполниться FIFO?
        log_write_char('\r');
        log_write_char('\n');
        //TODO для отладки сигнал: FE 55 F8 1F 55 40 в NRZI, первый байт - флаг 0х7E
        for (uint16_t i = 0; i < read_bytes; i++) {
            log_write_hex(buff[i]);
        }
        log_write_char('\r');
        log_write_char('\n');
    }
    
    power_tx_rx_off(); 
    return 0;
}

