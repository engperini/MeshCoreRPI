#!/usr/bin/env bash
# Prepare a Raspberry Pi for RaspberryPiMC.
#
# SPI radios (Waveshare SX1262 HAT, RPI_RADIO_DRIVER=sx1262 / sx127x) need the SPI bus enabled.
# USB radios (sx1262_ch341 / meshstick, sx1262_xiao / xiao) do not: SPI is left untouched.
# The driver is read from /etc/raspberrypimc/*.env (installed packages) or RaspberryPiMC/.env.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$EUID" -ne 0 ]]; then
  echo "Run as root: sudo bash RaspberryPiMC/setup_env.sh"
  exit 1
fi

mkdir -p "$SCRIPT_DIR/bin" "$SCRIPT_DIR/logs"
if [[ ! -f "$SCRIPT_DIR/.env" ]]; then
  cp "$SCRIPT_DIR/.env.example" "$SCRIPT_DIR/.env"
fi

radio_driver() {
  local f d
  for f in /etc/raspberrypimc/companion.env /etc/raspberrypimc/repeater.env "$SCRIPT_DIR/.env"; do
    [[ -f "$f" ]] || continue
    d="$(grep -E '^RPI_RADIO_DRIVER=' "$f" | tail -n1 | cut -d'=' -f2- | tr -d '"'"'"' ' || true)"
    if [[ -n "$d" ]]; then
      echo "$d"
      return
    fi
  done
  echo "sx1262"
}

boot_config() {
  if [[ -f /boot/firmware/config.txt ]]; then
    echo /boot/firmware/config.txt
  elif [[ -f /boot/config.txt ]]; then
    echo /boot/config.txt
  fi
}

DRIVER="$(radio_driver)"
NEED_REBOOT=0

case "$DRIVER" in
  sx1262_ch341|meshstick|sx1262_xiao|xiao)
    echo "Radio driver: $DRIVER (USB) - SPI not needed."
    ;;
  *)
    echo "Radio driver: $DRIVER (SPI HAT) - checking SPI."
    if ls /dev/spidev* >/dev/null 2>&1; then
      echo "SPI already enabled."
    elif command -v raspi-config >/dev/null 2>&1; then
      raspi-config nonint do_spi 0
      NEED_REBOOT=1
      echo "SPI enabled with raspi-config."
    else
      CFG="$(boot_config)"
      if [[ -n "$CFG" ]]; then
        if grep -qE '^\s*#?\s*dtparam=spi=' "$CFG"; then
          sed -i -E 's/^\s*#?\s*dtparam=spi=.*/dtparam=spi=on/' "$CFG"
        else
          echo "dtparam=spi=on" >> "$CFG"
        fi
        NEED_REBOOT=1
        echo "SPI enabled in $CFG."
      else
        echo "WARNING: could not find the boot config.txt; enable SPI manually."
      fi
    fi
    ;;
esac

echo "RaspberryPiMC environment prepared in $SCRIPT_DIR"
if [[ "$NEED_REBOOT" -eq 1 ]]; then
  echo "SPI was just enabled: reboot once before the first run."
fi
