#ifndef SX1278_H
#define SX1278_H

#include <stdint.h>
#include "base_types.h"

/* ---------------- Константы ---------------- */

/** Частоты AIS (Гц) */
#define AIS_FREQ_LOWER_HZ 161975000UL  ///< Нижний канал AIS
#define AIS_FREQ_UPPER_HZ 162025000UL  ///< Верхний канал AIS

/** Частота кварца SX1278 в Гц */
#define SX1278_CRYSTAL_HZ 26000000UL //32000000UL  ///Используется для пересчета регистров частоты

/** Полоса пропускания FIR фильтра в Гц */
#define FIR_BANDWIDTH 25000

//длина принимаемого пакета в байтах в пакетном режиме
#define FIXED_PACKET_LEN         40 

//время ожидания заполнения FIFO в мс (по факту с погрешностью)
#define PAYLOAD_READY_TIMEOUT_MS 100

//версия прошивки sx1278
#define SX1278_VERSION 0x12

/** Задержка в мс для стабилизации переходных процессов */
#define HARDWARE_PAUSE_MS 10

/** порт и пин MCU для управления пином RESET у sx1278 */
#define MCU_RESET_PORT           3 //P3.2
#define MCU_RESET_PIN            2 


/** синхрослово - преамбула и флаг HDLC в кодировке NRZI */
static const uint8_t SYNC_WORD[] = {0xCC, 0xCC, 0xCC, 0xFE}; //либо {0x33, 0x33, 0x33, 0x01}

/* ---------------- Типы ---------------- */

/** Режим работы приёма */
typedef enum {
    rx_mode_packet,      ///< Пакетный режим: читаем из FIFO после прихода полного пакета
    rx_mode_continuous   ///< Непрерывный режим: читаем по прерыванию DIO2
} sx_rx_mode_t;

/* ---------------- API драйвера ---------------- */

/**
 * Инициализация SX1278 для приёма AIS
 * @param mode - режим работы приёма (packet/continuous)
 * @param frequency_hz - частота AIS (например, AIS_FREQ_LOWER_HZ)
 * @return en_result_t - статус инициализации
 *   Ok - успешно
 *   ErrorInvalidParameter - неверный параметр
 *   ErrorUninitialized - ошибка при инициализации SPI или чипа
 */
en_result_t sx1278_init_rx_ais(sx_rx_mode_t mode, uint32_t frequency_hz);

/**
 * Полный сброс радиочипа и повторная настройка, 
 * @return en_result_t - статус сброса 
 *   Ok - успешно
 *   Error - общая ошибка
 */
en_result_t sx1278_reset();

/**
 * Проверка наличия ошибки радиочипа
 * @param mode - текущий режим приёма
 * @return en_result_t
 *   Ok - ошибок нет
 *   ErrorTimeout - ошибка FIFO или таймаут SPI
 *   ErrorNotReady - чип не готов к приёму
 */
en_result_t sx1278_has_error(sx_rx_mode_t mode);

/**
 * Чтение данных из FIFO в пакетном режиме
 * @param buf - указатель на буфер для данных
 * @param len - размер буфера
 * @param actual - количество реально считанных байт
 * @return en_result_t
 *   Ok - данные считаны успешно
 *   ErrorBufferFull - буфер недостаточен
 *   ErrorNotReady - пакет ещё не готов
 */
en_result_t sx1278_read_packet(uint8_t* buf, uint16_t const len, uint16_t* actual);

/**
 * Чтение данных при непрерывном режиме по DIO2
 * @param buf - указатель на буфер для данных
 * @param len - размер буфера
 * @param actual - количество реально считанных байт
 * @return en_result_t
 *   Ok - данные считаны успешно
 *   ErrorTimeout - ошибка или таймаут SPI
 *   ErrorNotReady - данных пока нет
 */
en_result_t sx1278_read_continuous(uint8_t* buf, uint16_t const len, uint16_t* actual);

#endif /* SX1278_H */