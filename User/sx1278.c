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
#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_BITRATE_MSB          0x02
#define REG_BITRATE_LSB          0x03
#define REG_FDEV_MSB             0x04
#define REG_FDEV_LSB             0x05
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_RX_BW                0x12
#define REG_RX_CONFIG            0x0D
#define REG_OOK_PEAK             0x14
#define REG_PREAMBLE_DETECT      0x1F
#define REG_SYNC_CONFIG          0x27
#define REG_SYNC_VALUE1          0x28
#define REG_PACKET_CONFIG1       0x30
#define REG_PACKET_CONFIG2       0x31
#define REG_PAYLOAD_LENGTH       0x32
#define REG_FIFO_THRESH          0x35
#define REG_IRQ_FLAGS2           0x3F
#define REG_VERSION              0x42



/* ---------------- Конфигурации ---------------- */
#define SPI_WRITE_BIT          (1U << 7U)  /* MSB=1 для записи */
#define SPI_READ_MASK          0x7FU       /* MSB=0 для чтения */

// Биты флагов в REG_IRQ_FLAGS2
#define IRQ2_PAYLOAD_READY       (1 << 2)   // Пакет принят
#define IRQ2_FIFO_OVERRUN        (1 << 4)   // Переполнение FIFO
#define IRQ2_FIFO_EMPTY          (1 << 6)   // FIFO пуст
#define IRQ2_FIFO_FULL           (1 << 7)   // FIFO заполнен

// Адреса LF-регистров (доступны при LowFrequencyModeOn = 1)
#define REG_AGC_REF_LF           0x61
#define REG_AGC_THRESH1_LF       0x62
#define REG_AGC_THRESH2_LF       0x63
#define REG_AGC_THRESH3_LF       0x64
#define REG_PLL_LF               0x70

// Биты режимов работы чипа
#define OP_MODE_LONGRANGE_MASK   0x00
#define OP_MODE_MODULATION_FSK_MASK   0x00
#define OP_MODE_LOW_FREQ_ON      (1 << 3) //1 Low Frequency Mode (access to LF test registers)/0 High Frequency Mode (access to HF test registers)
#define OP_MODE_SLEEP            0x00
#define OP_MODE_STDBY            0x01
#define OP_MODE_RX               0x05

#define AFC_AUTO_ON_BIT          (1 << 4) //0 No AFC performed at receiver startup/1 AFC is performed at each receiver startup
#define AGC_AUTO_ON_BIT          (1 << 3) //0 LNA gain forced by the LnaGain Setting/1 LNA gain is controlled by the AGC
#define RX_TRIGGER_NONE          0 //AFC_AUTO_ON_BIT и AGC_AUTO_ON_BIT должны быть 0, см таблицу 24 Receiver Startup Options
#define RX_TRIGGER_RSSI          0x01 //AGC или AGC & AFC, отдельно AFC нельзя

#define PREAMBLE_DETECT_OFF      0x00
#define SYNC_CONFIG_OFF          0x00
#define BITRATE_9600             9600 
#define FDEV_AIS                 2400UL /* Девиация для AIS (±2,4 кГц) */

// Значения по умолчанию для LF-диапазона (из Table 43)
#define AGC_REF_LF_DEFAULT        0x19
#define AGC_THRESH1_LF_DEFAULT    0x0C
#define AGC_THRESH2_STEP2_DEFAULT 0x04 // AgcStep2 (биты 7-4 в 0x63)
#define AGC_THRESH2_STEP3_DEFAULT 0x0B // AgcStep3 (биты 3-0 в 0x63)
#define AGC_THRESH3_STEP4_DEFAULT 0x0C // AgcStep4 (биты 7-4 в 0x64)
#define AGC_THRESH3_STEP5_DEFAULT 0x0C // AgcStep5 (биты 3-0 в 0x64)

// Зарезервированные биты в RegPllLf (должны быть всегда 0x10)
#define PLL_LF_RESERVED_BITS      0x10

#define RX_BW_MANT_COUNT    3   // Количество возможных значений мантиссы
#define RX_BW_EXP_MIN       1   // Минимальное значение экспоненты (согласно даташиту)
#define RX_BW_EXP_MAX       7   // Максимальное значение экспоненты

#define BIT_SYNC_ON              (1 << 5) //0 Bit Sync disabled (not possible in Packet mode)/1 Bit Sync enabled
#define PACKET_FORMAT_FIXED      0 //(1 << 7) 0 Fixed length/1 Variable length
#define PACKET_MODE_BIT          (1 << 6) //0 Continuous mode/1 Packet mode
#define SYNC_WORD_ON_BIT         (1 << 4) //Enables the Sync word generation and detection: 0 Off/1 On


//значения мантиссы и экспоненты для регистра RegRxBw для настройки FIR, см детали 3.5.6. Channel Filter
static const uint16_t MANT_VALUES[] = {16, 20, 24};
static const uint8_t  MANT_CODES[]   = {0, 1, 2};

// Возможные значения полосы ФАПЧ (биты 7-6) в регистре RegPllLf (0x70)
typedef enum {
    PLL_BW_75_KHZ   = 0x00,   // 00
    PLL_BW_150_KHZ  = 0x40,   // 01 (сдвинуто на 6 бит)
    PLL_BW_225_KHZ  = 0x80,   // 10
    PLL_BW_300_KHZ  = 0xC0    // 11
} pll_bandwidth_t;



#define TAG "SX"

//флаг инициализации шины SPI
static boolean_t is_spi_init = FALSE;
//флаг инициализации пина MCU для управления Reset
static boolean_t is_reset_pin_init = FALSE;




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
    // Clock source, PCLK/16
    SPIConfig.u8ClkDiv = SpiClkDiv16;
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
    __NOP(); __NOP();
    Spi_TxRx(addr | SPI_WRITE_BIT);  // MSB=1 -> запись
    Spi_TxRx(val);
    __NOP(); __NOP();
    SPI_SetCsHigh();
}


//TODO по хорошему нужно возвращать результат/ошибку чтения, а само значение записывать в переданный буфер
static uint8_t sx1278_read_reg(uint8_t addr)
{
    uint8_t val;
    SPI_SetCsLow();
    __NOP(); __NOP();
    Spi_TxRx(addr & SPI_READ_MASK); // MSB=0 -> чтение
    val = Spi_TxRx(0x00);
    __NOP(); __NOP();
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
    //включаем пакетный режим
    en_result_t res = sx1278_write_reg_safe(REG_PACKET_CONFIG2, PACKET_MODE_BIT);
    if (res != Ok) {
        return res;
    }
    //включаем фиксированную длину пакета
    //TODO провести эксперимент с безлимитной длиной пакета
    //Unlimited Length Packet Format: bit PacketFormat is set to 0 and PayloadLength is set to 0
    res = sx1278_write_reg_safe(REG_PACKET_CONFIG1, PACKET_FORMAT_FIXED);
    if (res != Ok) {
        return res;
    }
    //устанавливаем длину пакета равную FIXED_PACKET_LEN
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
    uint8_t sync_size = sync_word_len - 1; //на один байт меньше размера синхрослова для ioHomeOn=0
    res = sx1278_write_reg_safe(REG_SYNC_CONFIG, SYNC_WORD_ON_BIT | sync_size);
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


/* Непрерывный режим (Continuous) 
* выключить бит синхронизатор (регистр RegOokPeak(0x14)) и детектор синхрослова
*/
static en_result_t sx1278_set_continuous_mode(void)
{
    uint8_t v = sx1278_read_reg(REG_PACKET_CONFIG2);
    v  &= ~PACKET_MODE_BIT;           // bit6 = 0 для непрерывного режима
    en_result_t res = sx1278_write_reg_safe(REG_PACKET_CONFIG2, v);
    if (res != Ok) {
        return res;
    }

    v = sx1278_read_reg(REG_OOK_PEAK);
    v  &= ~BIT_SYNC_ON;           // bit5 = 0 для выключения бит синхронизатора
    res = sx1278_write_reg_safe(REG_OOK_PEAK, v);
    if (res != Ok) {
        return res;
    }

    v = sx1278_read_reg(REG_SYNC_CONFIG);
    v  &= ~SYNC_WORD_ON_BIT;           // bit4 = 0 для выключения поиска синхрослова
    return sx1278_write_reg_safe(REG_SYNC_CONFIG, v);
}



/**
 * Установка полосы пропускания FIR фильтра, зависит от частоты кварца
 * см детали 3.5.6. Channel Filter и Table 40 Available RxBw Settings
 * @return ErrorInvalidMode если не получится подобрать необходимые значения, 
 * иначе - статус записи в регистр
 */
en_result_t sx1278_set_rx_bandwidth(uint32_t target_bw_hz) {
    uint32_t xtal = SX1278_CRYSTAL_HZ;
    uint8_t best_mant_code = 0;
    uint8_t best_exp = 0;
    uint32_t best_diff = UINT32_MAX;
    int found = 0;

    for (int m = 0; m < RX_BW_MANT_COUNT; m++) {
        uint32_t mant = MANT_VALUES[m];
        uint8_t mant_code = MANT_CODES[m];
        for (int exp = RX_BW_EXP_MIN; exp <= RX_BW_EXP_MAX; exp++) {
            uint32_t divisor = mant * (1UL << (exp + 2));
            uint32_t bw_hz = xtal / divisor;
            uint32_t diff = (bw_hz >= target_bw_hz) ? (bw_hz - target_bw_hz) : (target_bw_hz - bw_hz);

            if (!found || diff < best_diff) {
                best_diff = diff;
                best_mant_code = mant_code;
                best_exp = exp;
                found = 1;
            }
        }
    }

    if (!found) return ErrorInvalidMode;

    uint8_t reg_value = (best_mant_code << 3) | best_exp;
    return sx1278_write_reg_safe(REG_RX_BW, reg_value);
}



/**
 * @brief Настройка LF-регистров с возможностью выбора полосы ФАПЧ.
 * @param pll_bw - желаемая полоса ФАПЧ (например, PLL_BW_75_KHZ).
 * @return en_result_t Ok при успехе.
 */
en_result_t sx1278_configure_lf_registers(pll_bandwidth_t pll_bw) {
    en_result_t res;
    uint8_t reg_val; 

    // --- Шаг 1: Убеждаемся, что мы в режиме LF ---
    uint8_t op_mode = sx1278_read_reg(REG_OP_MODE);
    if (!(op_mode & OP_MODE_LOW_FREQ_ON)) {
        return ErrorInvalidMode;
    }

    // --- Шаг 2: Настройка AGC ---
    res = sx1278_write_reg_safe(REG_AGC_REF_LF, AGC_REF_LF_DEFAULT);
    if (res != Ok) return res;

    res = sx1278_write_reg_safe(REG_AGC_THRESH1_LF, AGC_THRESH1_LF_DEFAULT);
    if (res != Ok) return res;

    reg_val = (AGC_THRESH2_STEP2_DEFAULT << 4) | AGC_THRESH2_STEP3_DEFAULT;
    res = sx1278_write_reg_safe(REG_AGC_THRESH2_LF, reg_val);
    if (res != Ok) return res;

    reg_val = (AGC_THRESH3_STEP4_DEFAULT << 4) | AGC_THRESH3_STEP5_DEFAULT;
    res = sx1278_write_reg_safe(REG_AGC_THRESH3_LF, reg_val);
    if (res != Ok) return res;

    // --- Шаг 3: Настройка PLL ---
    // Формируем байт: старшие 2 бита = pll_bw, младшие 6 бит = зарезервированные 0x10
    reg_val = pll_bw | PLL_LF_RESERVED_BITS;
    res = sx1278_write_reg_safe(REG_PLL_LF, reg_val);
    if (res != Ok) return res;

    return Ok;
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
        
    en_result_t res;
    /* 0. Сброс чипа */
    if (Ok != (res = reset_init())) {
        LOGE(TAG, res);
        return ErrorUninitialized;       
    }
    if (Ok != (res = sx1278_reset())) {
        LOGE(TAG, res);
        return ErrorUninitialized;         
    }
    LOGI(TAG, 0);

    /* 1. Инициализация SPI */
    if (Ok != (res = sx1278_spi_init())) {
        LOGE(TAG, res);
        return ErrorUninitialized;
    }

    uint8_t ver;
    if (SX1278_VERSION != (ver = sx1278_read_reg(REG_VERSION))) {
        LOGE(TAG, ver);
        return ErrorUninitialized; 
    }
    LOGI(TAG, 1);

    /* 2. софт ресет радиочипа через погружение в сон */
    if (Ok != (res = sx1278_write_reg_safe(REG_OP_MODE, OP_MODE_SLEEP))) {
        LOGE(TAG, res);
        return ErrorUninitialized;        
    }
    delay1ms(HARDWARE_PAUSE_MS);
    if (Ok != (res = sx1278_write_reg_safe(REG_OP_MODE, OP_MODE_LOW_FREQ_ON | OP_MODE_STDBY))) {
        LOGE(TAG, res);
        return ErrorUninitialized; 
    }
    LOGI(TAG, 2);

    /* 3. Частота */
    if (Ok != (res = sx1278_set_frequency(frequency_hz))) {
        LOGE(TAG, res);
        return ErrorUninitialized;
    }
    LOGI(TAG, 3);

    /* 4. Битрейт */
    if (Ok != (res = sx1278_set_bitrate(BITRATE_9600))) {
        LOGE(TAG, res);
        return ErrorUninitialized;
    }
    LOGI(TAG, 4);

    /* 5. Девиация */
    if (Ok != (res = sx1278_set_deviation(FDEV_AIS))) {
        LOGE(TAG, res);
        return ErrorUninitialized;
    }
    LOGI(TAG, 5);

    //TODO установка усиления LNA - регистр RegLna (0x0C), по умолчанию максимальное усиление

    /* 6. Настройка режимов приёма */
    //отключаем детектор преамбулы
    //The AGC settings supersede this bit during the startup / AGC phase.
    if(Ok != (res = sx1278_write_reg_safe(REG_PREAMBLE_DETECT, PREAMBLE_DETECT_OFF))) {
        LOGE(TAG, res);
        return ErrorUninitialized;      
    }
    LOGI(TAG, 61);

    //TODO пока выключаем AFC и AGC по RSSI
    if(Ok != (res = sx1278_write_reg_safe(REG_RX_CONFIG, RX_TRIGGER_NONE))) {
        LOGE(TAG, res);
        return ErrorUninitialized;      
    }
    LOGI(TAG, 62);

    //настраиваем полосу пропускания FIR фильтра
    if(Ok != (sx1278_set_rx_bandwidth(FIR_BANDWIDTH))) {
        LOGE(TAG, res);
        return ErrorUninitialized;         
    }
    LOGI(TAG, 63);

    //настраиваем LF регистры согласно Table 43 Low Frequency Additional Registers
    if(Ok != (res = sx1278_configure_lf_registers(PLL_BW_75_KHZ))) {
        return ErrorUninitialized;        
    }
    LOGI(TAG, 64);

    if (mode == rx_mode_packet)
    {
        /* 
        Пакетный режим: отключен детектор преамбулы, аппаратно включен бит-синхронизатор 
        включаем поиск синхрослова 
        загружаем синхрослово NRZI 0xCC 0xCC 0xCC 0xFE - преамбула+флаг HDLC пакета в NRZI кодировке
        */
        if (Ok != (res = sx1278_init_packet_mode())) {
            LOGE(TAG, res);
            return ErrorUninitialized; 
        }
    }
    else if (mode == rx_mode_continuous) 
    {   /* 
        Непрерывный режим: отключен детектор преамбулы, 
        выключаем поиск синхрослова и бит-синхронизатор 
        */
        if (Ok != (res = sx1278_set_continuous_mode())) {
            LOGE(TAG, res);
            return ErrorUninitialized; 
        }       
    }
    else {
        return ErrorInvalidParameter;
    }
    LOGI(TAG, 6);



    /* 7. Перевод в режим приёма GMSK */
    if(Ok != (res = sx1278_write_reg_safe(REG_OP_MODE, 
            OP_MODE_LONGRANGE_MASK | OP_MODE_MODULATION_FSK_MASK | 
            OP_MODE_LOW_FREQ_ON | OP_MODE_RX))) {
        LOGE(TAG, res);
        return ErrorUninitialized;
    }
    LOGI(TAG, 7);

    return Ok;
}


en_result_t sx1278_reset() {
    GPIO_SetPinOutLow(MCU_RESET_PORT, MCU_RESET_PIN);
    delay1ms(HARDWARE_PAUSE_MS);
    GPIO_SetPinOutHigh(MCU_RESET_PORT, MCU_RESET_PIN);
    delay1ms(HARDWARE_PAUSE_MS);
    return Ok;
}


en_result_t sx1278_read_packet(uint8_t* buf, uint16_t const len, uint16_t* actual) {
    // 0. Проверка входных параметров
    if (buf == NULL || actual == NULL) {
        return ErrorInvalidParameter;
    }

    uint16_t const read_len = len < FIXED_PACKET_LEN ? len : FIXED_PACKET_LEN;
    if (read_len <= 0) {
        return ErrorInvalidParameter;
    }

    // Изначально actual = 0 (означает, что данных не прочитано)
    *actual = 0;

    /* 1. Ожидание флага PayloadReady (RegIrqFlags2, бит 2) */
    uint32_t timeout_ms = PAYLOAD_READY_TIMEOUT_MS;
    uint8_t irq_flags2 = 0U;
    
    while (TRUE) {
        irq_flags2 = sx1278_read_reg(REG_IRQ_FLAGS2);
        
        if (((irq_flags2 & IRQ2_PAYLOAD_READY) != 0U) || ((irq_flags2 & IRQ2_FIFO_FULL) != 0U)) {
            break;  /* PayloadReady установлен */
        }
        
        /* Проверка на FIFO Overrun (ошибка переполнения) */
        if ((irq_flags2 & IRQ2_FIFO_OVERRUN) != 0U) {
            /* Очистить флаг overrun записью 1 */
            sx1278_write_reg(REG_IRQ_FLAGS2, IRQ2_FIFO_OVERRUN);
            return ErrorUninitialized;
        }
        timeout_ms--;
        if (0 >= timeout_ms) {
            return ErrorTimeout;
        }
        delay1ms(1U);
    }

    /* 2. Чтение FIXED_PACKET_LEN байт из FIFO (адрес 0x00) */
    /* По даташиту Section 2.2 (SPI Interface):
     * "FIFO access: if the address byte corresponds to the address of the FIFO,
     *  then succeeding data byte will address the FIFO. The address is not 
     *  automatically incremented but is memorized and does not need to be sent 
     *  between each data byte."
     * 
     * Это означает BURST чтение с одним адресным байтом в начале
     */
    SPI_SetCsLow();
    __NOP();__NOP();
    
    /* Адресный байт: MSB=0 (чтение), адрес 0x00 (FIFO) */
    Spi_TxRx(REG_FIFO & SPI_READ_MASK);
    
    /* Чтение данных байт за байтом без повторной отправки адреса */
    uint16_t i = 0U;
    for (; i < read_len; i++) {
        buf[i] = Spi_TxRx(0x00U);
        
    }
    __NOP();__NOP();
    SPI_SetCsHigh();

    //После чтения сбросить флаг IRQ2_PAYLOAD_READY
    sx1278_write_reg(REG_IRQ_FLAGS2, IRQ2_PAYLOAD_READY);

    *actual = i + 1;
    // Всё хорошо
    return Ok;
}