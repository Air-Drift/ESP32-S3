#!/usr/bin/env bash
# Package a build as release images:
#   dist/airdrift-<env>-<version>-full.bin   the whole flash (bootloader, partition table,
#                                            boot selector, app): write it at 0x0 on a new board
#   dist/airdrift-<env>-<version>.bin        the app only, for the Device tab's firmware update
#   dist/airdrift-<env>[-full].bin           the same two under fixed names, which the radio's own
#                                            update and the README fetch through .../releases/latest/download/
# The app image is rebuilt from the ELF and its checksum verified, because
# esptool 5.4 now and then writes one that will not boot.
#   scripts/release-images.sh <env> [version]
set -euo pipefail
ENV=${1:?env}; cd "$(dirname "$0")/.."
VERSION=${2:-$(sed -n 's/^#define AD_VERSION *"\([^"]*\)".*/\1/p' include/config.h)}
# esptool 5 (hyphenated commands): the one on PATH (pip install esptool, as the
# release workflow does), else PlatformIO's own copy.
if command -v esptool >/dev/null; then ESPTOOL=(esptool)
else ESPTOOL=(~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py); fi
B=".pio/build/$ENV"; OUT=dist; mkdir -p "$OUT"
APP="$OUT/airdrift-$ENV-$VERSION.bin"; FULL="$OUT/airdrift-$ENV-$VERSION-full.bin"
for try in 1 2 3 4 5; do
  "${ESPTOOL[@]}" --chip esp32s3 elf2image --flash-mode dio --flash-freq 80m --flash-size 16MB -o "$APP" "$B/firmware.elf" >/dev/null
  info=$("${ESPTOOL[@]}" --chip esp32s3 image-info "$APP" 2>/dev/null || true)
  if grep -qE "^Checksum: 0x[0-9a-f]+ \(valid\)" <<<"$info"; then break; fi
  echo "image checksum invalid, regenerating"; [ "$try" = 5 ] && { echo "$info" | grep -i checksum; exit 1; }
done
BOOT0=$(find ~/.platformio/packages/framework-arduinoespressif32 -name boot_app0.bin | head -1)
"${ESPTOOL[@]}" --chip esp32s3 merge-bin --flash-mode dio --flash-freq 80m --flash-size 16MB -o "$FULL" \
  0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT0" 0x10000 "$APP" >/dev/null
cp "$APP" "$OUT/airdrift-$ENV.bin"; cp "$FULL" "$OUT/airdrift-$ENV-full.bin"   # fixed names: .../releases/latest/download/
ls -l "$APP" "$FULL"
