// Copyright (c) Shinya Ishikawa. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full
// license information.

#ifndef BATTERYICON_H_
#define BATTERYICON_H_
#include <M5GFX.h>
#include <M5Unified.h>
#include "DrawContext.h"
#include "Drawable.h"

namespace m5avatar {

class BatteryIcon final : public Drawable {
 private:
  void drawBatteryIcon(lgfx::LovyanGFX *spi, uint32_t x, uint32_t y,
                       uint16_t fgcolor, uint16_t bgcolor,
                       BatteryIconStatus batteryIconStatus,
                       int32_t batteryLevel) {
    if (batteryLevel < 0) batteryLevel = 0;
    if (batteryLevel > 100) batteryLevel = 100;

    // Clear the exact overlay area first. This also removes stale fill or a
    // stale charging glyph when the state changes between GIF frames.
    spi->fillRect(x, y, 35, 15, bgcolor);
    spi->drawRect(x, y + 5, 5, 5, fgcolor);
    spi->drawRect(x + 5, y, 30, 15, fgcolor);
    int battery_width = 30 * batteryLevel / 100;
    if (battery_width > 0) {
      spi->fillRect(x + 5 + 30 - battery_width, y, battery_width, 15,
                    fgcolor);
    }
    if (batteryIconStatus == BatteryIconStatus::charging) {
      spi->fillTriangle(x + 20, y, x + 15, y + 8, x + 20, y + 8, bgcolor);
      spi->fillTriangle(x + 18, y + 7, x + 18, y + 15, x + 23, y + 7, bgcolor);
      spi->drawLine(x + 20, y, x + 15, y + 8, fgcolor);
      spi->drawLine(x + 20, y, x + 20, y + 7, fgcolor);
      spi->drawLine(x + 18, y + 15, x + 23, y + 7, fgcolor);
      spi->drawLine(x + 18, y + 8, x + 18, y + 15, fgcolor);
    }
 }

 public:
  // constructor
  BatteryIcon() = default;
  ~BatteryIcon() = default;
  BatteryIcon(const BatteryIcon &other) = default;
  BatteryIcon &operator=(const BatteryIcon &other) = default;

  void drawOverlay(lgfx::LovyanGFX *spi, uint32_t x, uint32_t y,
                   uint16_t fgcolor, uint16_t bgcolor,
                   BatteryIconStatus batteryIconStatus,
                   int32_t batteryLevel) {
    if (batteryIconStatus != BatteryIconStatus::invisible) {
      drawBatteryIcon(spi, x, y, fgcolor, bgcolor, batteryIconStatus,
                      batteryLevel);
    }
  }

  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    if (ctx->getBatteryIconStatus() != BatteryIconStatus::invisible) {
      uint16_t primaryColor = ctx->getColorDepth() == 1 ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);
      uint16_t bgColor = ctx->getColorDepth() == 1 ? ERACER_COLOR : ctx->getColorPalette()->get(COLOR_BACKGROUND);
      int32_t batteryLevel = ctx->getBatteryLevel();
      drawBatteryIcon(spi, rect.getLeft(), rect.getTop(), primaryColor,
                      bgColor, ctx->getBatteryIconStatus(), batteryLevel);
    }
  };

};

}  // namespace m5avatar

#endif  // BATTERYICON_H_
