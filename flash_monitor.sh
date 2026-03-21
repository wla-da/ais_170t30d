#!/bin/bash
# Настройки для прошивки MCU HC32L110 и прослушивания UART порта с отладочными сообщениями от MCU
PORT="/dev/ttyUSB0"          # Ваш порт (уточните через ls /dev/ttyUSB*)
BAUD=115200
TARGET="hc32l110"

echo "==> Прошивка..."
make flash || { echo "Ошибка прошивки"; exit 1; }

echo "==> Запускаем монитор порта $PORT (скорость $BAUD)"
echo "Для выхода нажмите Ctrl+T Q"
#tio $PORT -b $BAUD

(sleep 1 && pyocd reset -t $TARGET) &   # Сброс MCU через 1 секунду после старта tio
tio $PORT -b $BAUD                   