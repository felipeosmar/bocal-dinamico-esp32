#!/bin/bash
# Flash helper script for bocal-dinamico-esp32
# Supports partial flashing to preserve user configuration

set -e

PORT="${PORT:-/dev/ttyUSB0}"
BAUD="${BAUD:-460800}"

show_help() {
    echo "Usage: $0 [OPTION]"
    echo ""
    echo "Flash options:"
    echo "  all         Full flash (factory reset) - erases user config"
    echo "  app         Flash only firmware (preserves www and userdata)"
    echo "  update      Flash firmware + web interface (preserves userdata/config)"
    echo "  www         Flash only web interface"
    echo ""
    echo "Environment variables:"
    echo "  PORT        Serial port (default: /dev/ttyUSB0)"
    echo "  BAUD        Baud rate (default: 460800)"
    echo ""
    echo "Examples:"
    echo "  $0 update              # Normal update (keeps user config)"
    echo "  $0 all                 # Factory reset"
    echo "  PORT=/dev/ttyACM0 $0 app  # Flash app to specific port"
}

check_build() {
    if [ ! -f "build/bocal-dinamico.bin" ]; then
        echo "Error: Build not found. Run 'idf.py build' first."
        exit 1
    fi
}

flash_all() {
    echo "=== Full Flash (Factory Reset) ==="
    echo "WARNING: This will erase ALL user configuration!"
    read -p "Continue? (y/N) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        idf.py -p "$PORT" -b "$BAUD" flash
    fi
}

flash_app() {
    echo "=== Flashing Firmware Only ==="
    check_build
    idf.py -p "$PORT" -b "$BAUD" app-flash
}

flash_www() {
    echo "=== Flashing Web Interface Only ==="
    if [ ! -f "build/www.bin" ]; then
        echo "Error: www.bin not found. Run 'idf.py build' first."
        exit 1
    fi
    esptool.py -p "$PORT" -b "$BAUD" write_flash 0x1D0000 build/www.bin
}

flash_update() {
    echo "=== Update Flash (Firmware + Web Interface) ==="
    echo "User configuration will be PRESERVED"
    check_build

    # Flash app partition
    idf.py -p "$PORT" -b "$BAUD" app-flash

    # Flash www partition
    if [ -f "build/www.bin" ]; then
        echo "Flashing web interface..."
        esptool.py -p "$PORT" -b "$BAUD" write_flash 0x1D0000 build/www.bin
    fi

    echo "=== Update Complete ==="
}

case "$1" in
    all)
        flash_all
        ;;
    app)
        flash_app
        ;;
    www)
        flash_www
        ;;
    update)
        flash_update
        ;;
    -h|--help|help)
        show_help
        ;;
    *)
        show_help
        exit 1
        ;;
esac
