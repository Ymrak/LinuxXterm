#!/bin/bash


set -e

echo "=== Установка LinuxXterm ==="

if [ "$EUID" -ne 0 ]; then
    echo "Ошибка: запустите скрипт с правами root (sudo ./install.sh)"
    exit 1
fi

if [ ! -f "../build/linuxxterm" ]; then
    echo "Ошибка: бинарник не найден. Сначала выполните сборку:"
    echo "  cd build && cmake .. && make"
    exit 1
fi

echo "Установка бинарника..."
cp ../build/linuxxterm /usr/local/bin/linuxxterm
chmod +x /usr/local/bin/linuxxterm

echo "Установка иконки..."
mkdir -p /usr/share/icons/hicolor/128x128/apps
cp linuxxterm.svg /usr/share/icons/hicolor/128x128/apps/linuxxterm.svg

echo "Установка .desktop файла..."
mkdir -p /usr/share/applications
cp linuxxterm.desktop /usr/share/applications/linuxxterm.desktop

echo "Обновление кэша..."
gtk-update-icon-cache -f /usr/share/icons/hicolor/ || true
update-desktop-database /usr/share/applications/ || true

echo ""
echo "=== Установка завершена ==="
echo "LinuxXterm установлен в /usr/local/bin/linuxxterm"
echo "Иконка добавлена в меню приложений"
echo ""
echo "Для запуска используйте:"
echo "  linuxxterm"
echo "или найдите в меню приложений"
