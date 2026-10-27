#!/usr/bin/env bash
# MeshCoreRPI one-line installer.
#
#   curl -fsSL https://raw.githubusercontent.com/engperini/MeshCoreRPI/main/install.sh | sudo bash
#   curl -fsSL https://raw.githubusercontent.com/engperini/MeshCoreRPI/main/install.sh | sudo bash -s -- repeater
#
# Downloads the latest release package for this Pi, installs it with apt
# (pulling dependencies), and starts the service. Existing config in
# /etc/raspberrypimc/ is never overwritten.
set -euo pipefail

REPO="${MESHCORERPI_REPO:-engperini/MeshCoreRPI}"
ROLE="${1:-companion}"

if [[ "$ROLE" != "companion" && "$ROLE" != "repeater" ]]; then
  echo "Usage: install.sh [companion|repeater]" >&2
  exit 2
fi
if [[ $EUID -ne 0 ]]; then
  echo "Please run as root (pipe into: sudo bash)" >&2
  exit 1
fi

ARCH="$(dpkg --print-architecture)"
ENV_FILE="/etc/raspberrypimc/${ROLE}.env"
had_env=0
[[ -f "$ENV_FILE" ]] && had_env=1

echo "==> Looking up latest ${ROLE} package (${ARCH}) in ${REPO}"
url="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
  | grep -o "https://[^\"]*raspberrypimc-native-${ROLE}_[^\"]*_${ARCH}\.deb" | head -n1 || true)"
if [[ -z "$url" ]]; then
  echo "No ${ARCH} package for '${ROLE}' in the latest release." >&2
  echo "Build from source instead: https://github.com/${REPO}#build-from-source" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
deb="$tmp/$(basename "$url")"
echo "==> Downloading $(basename "$url")"
curl -fL --progress-bar -o "$deb" "$url"

echo "==> Installing"
apt-get update -qq
apt-get install -y "$deb" libusb-1.0-0

# Fresh companion install: let the companion app own the radio params (region),
# instead of forcing the template's EU values at every start.
if [[ $had_env -eq 0 && "$ROLE" == "companion" ]]; then
  sed -i -E 's/^(RPI_(FREQ_HZ|SF|BW_HZ|CR|TX_DBM)=)/# \1/' "$ENV_FILE"
  sed -i '1i # Radio params are set from the companion app (saved on the Pi).\n# Uncomment RPI_FREQ_HZ/RPI_SF/RPI_BW_HZ/RPI_CR/RPI_TX_DBM to force them at every start.' "$ENV_FILE"
fi

# Fresh install with a USB MeshStick (CH341, 1a86:5512) plugged in: select its driver.
meshstick=0
for d in /sys/bus/usb/devices/*; do
  if [[ -f "$d/idVendor" && "$(cat "$d/idVendor")" == "1a86" && "$(cat "$d/idProduct" 2>/dev/null)" == "5512" ]]; then
    meshstick=1
  fi
done
if [[ $meshstick -eq 1 ]]; then
  if [[ $had_env -eq 0 ]]; then
    echo "==> MeshStick detected: setting RPI_RADIO_DRIVER=sx1262_ch341 in $ENV_FILE"
    if grep -q '^RPI_RADIO_DRIVER=' "$ENV_FILE"; then
      sed -i 's/^RPI_RADIO_DRIVER=.*/RPI_RADIO_DRIVER=sx1262_ch341/' "$ENV_FILE"
    else
      echo 'RPI_RADIO_DRIVER=sx1262_ch341' >> "$ENV_FILE"
    fi
  else
    echo "==> MeshStick detected; keeping your existing $ENV_FILE"
    echo "    (set RPI_RADIO_DRIVER=sx1262_ch341 there if it is not set yet)"
  fi
fi

echo "==> Starting raspberrypimc-${ROLE}.service"
systemctl enable "raspberrypimc-${ROLE}.service" >/dev/null
systemctl restart "raspberrypimc-${ROLE}.service"
sleep 4
journalctl -u "raspberrypimc-${ROLE}.service" -n 30 --no-pager | grep -E "startup-config|radio-diag|fatal" || true

ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
echo
echo "Done. Config: $ENV_FILE  (edit, then: sudo systemctl restart raspberrypimc-${ROLE})"
if [[ "$ROLE" == "companion" ]]; then
  echo "Companion TCP: ${ip:-<pi-ip>}:5000   Web dashboard: http://${ip:-<pi-ip>}:8080"
else
  echo "Repeater CLI TCP: ${ip:-<pi-ip>}:5001   Web dashboard: http://${ip:-<pi-ip>}:8081"
fi
echo "Set your region's radio parameters before transmitting (see README)."
