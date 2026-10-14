#!/usr/bin/env bash
# Build an env, rebuild its image from the ELF (PlatformIO 6.2 sometimes
# writes one with a bad checksum), and flash it with esptool.
#   scripts/flash.sh waveshare-185 [/dev/ttyACM0]
set -e
ENV=${1:-waveshare-185}; PORT=${2:-/dev/ttyACM0}
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
B0=$(find ~/.platformio/packages/framework-arduinoespressif32 -name boot_app0.bin | head -1)
$PY $ESPTOOL --chip esp32s3 --port "$PORT" --baud 921600 write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 "$B0" 0x10000 fw.bin
