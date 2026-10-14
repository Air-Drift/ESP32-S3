#!/usr/bin/env bash
# Build an env and send the firmware to a radio over WiFi (no USB needed).
#   scripts/ota.sh waveshare-185 [airdrift.local]
set -e
ENV=${1:-waveshare-185}; HOST=${2:-airdrift.local}
cd "$(dirname "$0")/.."
for try in 1 2 3 4 5; do
  rm -rf ".pio/build/$ENV"
  if pio run -e "$ENV"; then break; fi
  echo "build attempt $try failed (PlatformIO fault?), retrying"
done
PY=~/.platformio/penv/bin/python; ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py
cd ".pio/build/$ENV"
# esptool 5.4 now and then writes an image with a bad checksum; check, and redo.
for try in 1 2 3 4 5; do
  $PY $ESPTOOL --chip esp32s3 elf2image --flash-mode dio --flash-freq 80m --flash-size 16MB -o fw.bin firmware.elf >/dev/null
  if $PY $ESPTOOL --chip esp32s3 image-info fw.bin | grep -qE "^Checksum: 0x[0-9a-f]+ \(valid\)"; then break; fi
  echo "image checksum invalid, regenerating"
done
echo "sending $(du -h fw.bin | cut -f1) to http://$HOST ..."
curl -sS --max-time 180 -F "firmware=@fw.bin" "http://$HOST/api/update" && echo
