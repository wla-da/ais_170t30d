/*
настройка радиочипа SX1278 для приема сигналов AIS:
-два канала 161,975 МГц и 162,025 МГц, 
-модуляця GMSK, 
-индекс модуляции h=0,5 
-BT=0,4 (произведение ширины полосы пропускания фильтра Гаусса на длительность одного бита), 
-битрейт R=9600 бит/с, 
-девиация частоты 2,4 кГц = h*R/2, 
-длительность пакета ~26 мс

Общая логика
а) инициализация приема
1. установка режима: пакетный/непрерывный
2. установка частоты, модуляции GMSK и тп

б) далее, чтение из FIFO для пакетного/чтение с DIO2 для непрерывного режима

в) в случае ошибки - сброс (reset) и настройка заново радиочипа
*/

#include "spi.h"
#include "gpio.h"
#include "ddl.h"

#include "sx1278.h"

#include "ulog.h"


/* ---------------- Регистры SX1278 ---------------- */
#define REG_OP_MODE              0x01
#define REG_BITRATE_MSB          0x02
#define REG_BITRATE_LSB          0x03
#define REG_FDEV_MSB             0x04
#define REG_FDEV_LSB             0x05
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PREAMBLE_DETECT      0x1F
#define REG_SYNC_CONFIG          0x27
#define REG_AFC_CONFIG           0x28
#define REG_FIFO_THRESH          0x35
#define REG_VERSION              0x42
#define REG_RX_CONFIG            0x0D
#define REG_SYNC_CONFIG          0x27
#define REG_SYNC_VALUE1          0x28
#define REG_PACKET_CONFIG1       0x30
#define REG_PACKET_CONFIG2       0x31
#define REG_PAYLOAD_LENGTH       0x32


/* ---------------- Конфигурации ---------------- */
#define OP_MODE_LONGRANGE_MASK   0x00
#define OP_MODE_MODULATION_FSK_MASK   0x00
#define OP_MODE_LOW_FREQ_ON_MASK 0x08 //1 Low Frequency Mode (access to LF test registers)/0 High Frequency Mode (access to HF test registers)
#define OP_MODE_SLEEP            0x00
#define OP_MODE_STDBY            0x01
#define OP_MODE_RX               0x05

#define PREAMBLE_DETECT_OFF      0x00
#define SYNC_CONFIG_OFF          0x00
#define AFC_AGC_RSSI             0x01
#define BITRATE_9600             9600 

#define PACKET_FORMAT_FIXED      (1 << 7) //0 Fixed length/1 Variable length
#define PACKET_MODE_BIT          (1 << 6) //0 Continuous mode/1 Packet mode
#define SYNC_WORD_ON_BIT         (1 << 4)
#define FIXED_PACKET_LEN         64 //длина пакета в байтах в пакетном режиме

//версия прошивки sx1278
#define SX1278_VERSION 0x12


/* Девиация для AIS (±2,4 кГц) */
#define FDEV_AIS                 2400UL


#define TAG "SX"

//флаг инициализации шины SPI
static boolean_t is_spi_init = FALSE;
//флаг инициализации пина MCU для управления Reset
static boolean_t is_reset_pin_init = FALSE;

//синхрослово - преамбула и флаг HDLC в кодировке NRZI
static const uint8_t SYNC_WORD[] = { 0xCC, 0xCC, 0xCC, 0xFE };


/* ---------------- Вспомогательные функции ---------------- */
static en_result_t reset_init() {
    if (is_reset_pin_init) {
        //ранее SPI был уже инициализирован
        return Ok;
    }
    en_result_t res = Gpio_InitIOExt(MCU_RESET_PORT, MCU_RESET_PIN, 
                        GpioDirOut, FALSE, FALSE, FALSE, FALSE); 
    if (res == Ok) {
        is_reset_pin_init = TRUE;
        return Ok;
    }
    else {
        return res;
    }    
}

static en_result_t sx1278_spi_init() {
    if (is_spi_init) {
        //ранее SPI был уже инициализирован
        return Ok;
    }

    stc_spi_config_t  SPIConfig;

    CLK_EnablePeripheralClk(ClkPeripheralSpi);
    CLK_EnablePeripheralClk(ClkPeripheralGpio);

    //настраиваем соответствующие пины MCU
    Gpio_SetFunc_SPI_CS_P14();
    Gpio_SetFunc_SPI_MOSI_P24();
    Gpio_SetFunc_SPI_MISO_P23();
    Gpio_SetFunc_SPI_CLK_P15();

    SPIConfig.bCPHA = SpiClockPhaseFirst;
    SPIConfig.bCPOL = SpiClockPolarityLow;
    // Turn off interrupts
    SPIConfig.bIrqEn = FALSE;
    // Master mode
    SPIConfig.bMasterMode = SpiMaster;
    // Clock source, PCLK/2
    SPIConfig.u8ClkDiv = SpiClkDiv2;
    // No callback
    SPIConfig.pfnIrqCb = NULL;
    en_result_t res = Spi_Init(&SPIConfig);
    if (res == Ok) {
        is_spi_init = TRUE;
        return Ok;
    }
    else {
        return res;
    }
}

static void sx1278_write_reg(uint8_t addr, uint8_t val)
{
    SPI_SetCsLow();
    Spi_TxRx(addr | 0x80);  // MSB=1 -> запись
    Spi_TxRx(val);
    SPI_SetCsHigh();
}

static uint8_t sx1278_read_reg(uint8_t addr)
{
    uint8_t val;
    SPI_SetCsLow();
    Spi_TxRx(addr & 0x7F); // MSB=0 -> чтение
    val = Spi_TxRx(0x00);
    SPI_SetCsHigh();
    return val;
}

static en_result_t sx1278_write_reg_safe(uint8_t addr, uint8_t val) {
    sx1278_write_reg(addr, val);
    //TODO нужно ли делать задержку перед чтением регистра?
    return (sx1278_read_reg(addr) == val) ? Ok : ErrorUninitialized;
}

static en_result_t sx1278_set_frequency(uint32_t frequency_hz)
{
    uint32_t frf = ((uint64_t)frequency_hz << 19) / SX1278_CRYSTAL_HZ;
    uint8_t msb = (frf >> 16) & 0xFF;
    uint8_t mid = (frf >> 8) & 0xFF;
    uint8_t lsb = frf & 0xFF;

    if ((sx1278_write_reg_safe(REG_FRF_MSB, msb) != Ok) ||
        (sx1278_write_reg_safe(REG_FRF_MID, mid) != Ok) ||
        (sx1278_write_reg_safe(REG_FRF_LSB, lsb) != Ok)) {
        return ErrorUninitialized;
    }
    return Ok;
}

static en_result_t sx1278_set_bitrate(uint32_t bitrate)
{
    uint16_t br_reg = (uint16_t)(SX1278_CRYSTAL_HZ / bitrate);
    if ((sx1278_write_reg_safe(REG_BITRATE_MSB, (br_reg >> 8) & 0xFF) != Ok) ||
        (sx1278_write_reg_safe(REG_BITRATE_LSB, br_reg & 0xFF) != Ok)) {
             return ErrorUninitialized;           
    }
    return Ok;
}

static en_result_t sx1278_set_deviation(uint16_t fdev_hz)
{
    uint16_t reg_val = (uint16_t)(((uint64_t)fdev_hz << 19) / SX1278_CRYSTAL_HZ);
    if ((sx1278_write_reg_safe(REG_FDEV_MSB, (reg_val >> 8) & 0xFF) != Ok)  ||
        (sx1278_write_reg_safe(REG_FDEV_LSB, reg_val & 0xFF) != Ok)) {
             return ErrorUninitialized;             
    }
    return Ok;
}



/* Пакетный режим (Packet)
* выключает проверку CRC, DcFree, фильтрацию по адресу
* включает фиксированную длину пакета равную FIXED_PACKET_LEN
* и устанавливает синхрослово
*/
static en_result_t sx1278_init_packet_mode(void)
{
    en_result_t res = sx1278_write_reg_safe(REG_PACKET_CONFIG2, PACKET_MODE_BIT);
    if (res != Ok) {
        return res;
    }
    res = sx1278_write_reg_safe(REG_PACKET_CONFIG1, PACKET_FORMAT_FIXED);
    if (res != Ok) {
        return res;
    }
    res = sx1278_write_reg_safe(REG_PAYLOAD_LENGTH, FIXED_PACKET_LEN);
    if (res != Ok) {
        return res;
    }
    /*
    рассчитываем длину синхрослова и значение 
    Size of the Sync word: (SyncSize + 1) bytes, (SyncSize) bytes if ioHomeOn=1
    для записи в регистр REG_SYNC_CONFIG
    */
    uint8_t sync_word_len = sizeof(SYNC_WORD);
    res = sx1278_write_reg_safe(REG_SYNC_CONFIG, SYNC_WORD_ON_BIT | (sync_word_len - 1));
    if (res != Ok) {
        return res;
    }
    //устанавливаем синхрослово
    for (uint8_t i = 0; i < sync_word_len; i++) {
        res = sx1278_write_reg_safe(REG_SYNC_VALUE1 + i, SYNC_WORD[i]);
        if (res != Ok) {
            return res;
        }
    }
    return Ok;
}


/* Непрерывный режим (Continuous) */
static en_result_t sx1278_set_continuous_mode(void)
{
    uint8_t v = sx1278_read_reg(REG_PACKET_CONFIG2);
    v |= PACKET_MODE_BIT;           // bit6 = 1
    return sx1278_write_reg_safe(REG_PACKET_CONFIG2, v);
}


/* ---------------- Основная функция ---------------- */
en_result_t sx1278_init_rx_ais(sx_rx_mode_t mode, uint32_t frequency_hz)
{
    if ((mode != rx_mode_packet) && (mode != rx_mode_continuous)) {
        return ErrorInvalidParameter;
    }
    if ((frequency_hz < AIS_FREQ_LOWER_HZ) || (frequency_hz > AIS_FREQ_UPPER_HZ)) {
        return ErrorInvalidParameter;
    }
        
    /* 0. Сброс чипа */
    if (reset_init() != Ok) {
        return ErrorUninitialized;       
    }
    if (sx1278_reset() != Ok) {
        return ErrorUninitialized;         
    }
    LOGI(TAG, 0);

    /* 1. Инициализация SPI */
    if (sx1278_spi_init() != Ok) {
        return ErrorUninitialized;
    }

    uint8_t ver = sx1278_read_reg(REG_VERSION);
    if (ver != SX1278_VERSION) {
        LOGE(TAG, ver);
        return ErrorUninitialized; 
    }
    LOGI(TAG, 1);

    /* 2. софт ресет радиочипа через погружение в сон */
    if (sx1278_write_reg_safe(REG_OP_MODE, OP_MODE_SLEEP) != Ok) {
        return ErrorUninitialized;        
    }
    delay1ms(2);
    if (sx1278_write_reg_safe(REG_OP_MODE, OP_MODE_STDBY) != Ok) {
        return ErrorUninitialized; 
    }
    LOGI(TAG, 2);

    /* 3. Частота */
    if (sx1278_set_frequency(frequency_hz) != Ok) {
        return ErrorUninitialized;
    }
    LOGI(TAG, 3);

    /* 4. Битрейт */
    if (sx1278_set_bitrate(BITRATE_9600) != Ok) {
        return ErrorUninitialized;
    }
    LOGI(TAG, 4);

    /* 5. Девиация */
    if (sx1278_set_deviation(FDEV_AIS) != Ok) {
        return ErrorUninitialized;
    }
    LOGI(TAG, 5);

    //TODO установка усиления LNA - регистр RegLna (0x0C), по умолчанию максимальное усиление

    /* 6. Настройка режимов приёма */
    //отключаем детектор преамбулы
    if(sx1278_write_reg_safe(REG_PREAMBLE_DETECT, PREAMBLE_DETECT_OFF) != Ok) {
        return ErrorUninitialized;      
    }
    //включаем AFC по RSSI
    sx1278_write_reg(REG_AFC_CONFIG, AFC_AGC_RSSI);

    if (mode == rx_mode_packet)
    {
        /* 
        Пакетный режим: отключен детектор преамбулы, аппаратно включен бит-синхронизатор 
        включаем поиск синхрослова 
        загружаем синхрослово NRZI 0xCC 0xCC 0xCC 0xFE - преамбула+флаг HDLC пакета в NRZI кодировке
        */
        if (sx1278_init_packet_mode() != Ok) {
            return ErrorUninitialized; 
        }
    }
    else if (mode == rx_mode_continuous) 
    {
        if (sx1278_set_continuous_mode() != Ok) {
            return ErrorUninitialized; 
        }       
        /* Непрерывный режим: отключен детектор преамбулы, выключаем поиск синхрослова и бит-синхронизатор */
        sx1278_write_reg(REG_SYNC_CONFIG, SYNC_CONFIG_OFF);
    }
    else {
        return ErrorInvalidParameter;
    }
    LOGI(TAG, 6);

    //TODO не забыть Table 43 Low Frequency Additional Registers

    /* 7. Перевод в режим приёма GMSK */
    if(sx1278_write_reg_safe(REG_OP_MODE, 
        OP_MODE_LONGRANGE_MASK | OP_MODE_MODULATION_FSK_MASK | 
        OP_MODE_LOW_FREQ_ON_MASK | OP_MODE_RX) != Ok) {
        return ErrorUninitialized;
    }
    LOGI(TAG, 7);

    return Ok;
}


en_result_t sx1278_reset() {
    GPIO_SetPinOutLow(MCU_RESET_PORT, MCU_RESET_PIN);
    delay1ms(1);
    GPIO_SetPinOutHigh(MCU_RESET_PORT, MCU_RESET_PIN);
    delay1ms(10);
    return Ok;
}