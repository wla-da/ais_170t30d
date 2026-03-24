#ifndef ULOG_H
#define ULOG_H

/*
компактный UART логгер для HC32L110
*/

#include <stdint.h>

#include "base_types.h"


/*
    Уровни логирования.
    Можно менять на этапе компиляции.
*/
#define LOG_LEVEL_NONE   0
#define LOG_LEVEL_ERROR  1
#define LOG_LEVEL_WARN   2
#define LOG_LEVEL_INFO   3
#define LOG_LEVEL_DEBUG  4

/*
    Текущий уровень логирования.
    Всё что выше — будет вырезано компилятором.
*/
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

/*
    Символы уровней логирования.
*/
#define LOG_CHAR_ERROR  'E'
#define LOG_CHAR_WARN   'W'
#define LOG_CHAR_INFO   'I'
#define LOG_CHAR_DEBUG  'D'


/*
    Базовые функции логирования
*/
en_result_t init_ulog();
void log_write_char(char c);
void log_write_str(char *s);
void log_write_hex(uint8_t h);
void log_write_u32(uint32_t value);
void log_write_i32(int32_t value);
void log_write_line(char level, char *tag, uint32_t value);


/*
    Макросы логирования
*/

#if LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOGE(tag,val) log_write_line(LOG_CHAR_ERROR,tag,val)
#else
#define LOGE(tag,val)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARN
#define LOGW(tag,val) log_write_line(LOG_CHAR_WARN,tag,val)
#else
#define LOGW(tag,val)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
#define LOGI(tag,val) log_write_line(LOG_CHAR_INFO,tag,val)
#else
#define LOGI(tag,val)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOGD(tag,val) log_write_line(LOG_CHAR_DEBUG,tag,val)
#else
#define LOGD(tag,val)
#endif

#endif