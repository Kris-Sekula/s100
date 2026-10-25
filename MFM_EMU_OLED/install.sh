#!/bin/bash
# Install the MFM OLED filename display on a BeagleBone running
# David Gesswein's MFM emulator.
#
#   1. Copy this directory's files to the BeagleBone (e.g. with scp).
#   2. On the BeagleBone, run:  sudo ./install.sh
#
# What it does:
#   - copies mfm_oled.py to /opt/mfm/oled/
#   - installs mfm-oled.service and enables + starts it
#
# It does NOT modify mfm_emu, its configuration, or any emulation file,
# and it needs NO network and NO apt/pip installs: the daemon uses
# python3's standard library only (raw /dev/i2c-* ioctls plus a
# built-in 5x7 font). This is deliberate - on the Debian 11 (bullseye)
# BeagleBone image, `apt install python3-pip` fails with 404s because
# the bullseye-security index references superseded python3.9 packages
# that the server no longer serves, and upgrading the system python on
# an emulator board is undesirable anyway.
set -e
cd "$(dirname "$0")"

if [ "$(id -u)" -ne 0 ]; then
  echo "Run as root (sudo ./install.sh)" >&2
  exit 1
fi

# Sanity check the display before installing the service: draw once.
# If this fails, the journal will not be much help either - fix the
# wiring/address first (see README).
echo "Drawing a test screen once..."
python3 mfm_oled.py --once || {
  echo "Test draw failed - display not reachable; not installing service." >&2
  echo "Check wiring and run:  sudo i2cdetect -y -r 2   (expect 3c)" >&2
  exit 1
}

mkdir -p /opt/mfm/oled
cp mfm_oled.py /opt/mfm/oled/mfm_oled.py
chmod 755 /opt/mfm/oled/mfm_oled.py
cp mfm-oled.service /etc/systemd/system/mfm-oled.service

systemctl daemon-reload
systemctl enable mfm-oled.service
systemctl restart mfm-oled.service

echo "Installed. Check with:  journalctl -u mfm-oled -f"
echo "And confirm the display shows the current emulation filename."
