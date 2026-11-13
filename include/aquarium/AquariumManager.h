// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <Arduino.h>

static constexpr uint8_t AQUARIUM_MAX_WEEDS = 5;
static constexpr uint8_t AQUARIUM_MAX_FISH = 6;

/**
 * @brief How fish choose their depth
 */
enum class FishSwim : uint8_t {
    Lanes,  // every fish keeps its own horizontal lane
    Free,   // fish pick any depth and may cross paths
};

struct AquariumWeed {
    int16_t axisX;
    uint8_t brackets;
};

/**
 * @brief Runtime-tunable geometry of the aquarium scene
 *
 * Every value can be changed from the web UI without rebuilding the firmware.
 */
struct AquariumConfig {
    int16_t arcHeight;      // height of a single bracket in pixels
    int16_t arcOffset;      // vertical shift between neighbouring brackets
    int16_t arcBulge;       // how far a bracket leans away from its axis
    int16_t arcStroke;      // bracket line thickness
    int16_t stagger;        // horizontal shift of each bracket above the base
    int16_t swayMax;        // sway of the topmost bracket, the base never moves
    int16_t swayPhaseStep;  // phase shift between brackets, 0 = rigid lean
    uint16_t swayStepMs;    // duration of one sine step, full cycle = 64 * this
    uint16_t color;         // RGB565 weed color
    uint8_t fishCount;
    int16_t fishScaleMin;   // glyph pixel scale of the smallest fish
    int16_t fishScaleMax;   // glyph pixel scale of the biggest fish
    int16_t fishSpeedMin;   // pixels per second of the slowest fish
    int16_t fishSpeedMax;   // pixels per second of the fastest fish
    int16_t fishBobMin;     // vertical bob amplitude of the calmest fish
    int16_t fishBobMax;     // vertical bob amplitude of the liveliest fish
    bool fishTurnAnim;      // play the flip animation instead of snapping around
    FishSwim fishSwim;      // lanes keep the fish apart, free lets them roam
    bool bubbles;           // fish let air bubbles go
    uint8_t bubbleChance;   // percent chance that a fish lets one go per bubble interval
    int16_t bubbleRise;     // how fast a bubble floats up, pixels per second
    uint16_t bubbleEveryMs; // how often a fish rolls for a bubble
    uint16_t weedGrowS;     // seconds it takes a bitten bracket to grow back
    bool weedEat;           // fish are allowed to bite brackets off the seaweed
    uint8_t fishEatChance;  // percent chance that a fish stops at a plant it passes over
    uint8_t weedCount;
    AquariumWeed weeds[AQUARIUM_MAX_WEEDS];
};

/**
 * @brief ASCII-style aquarium scene rendered straight to the panel
 *
 * No full frame buffer exists on the ESP8266, so the scene is composed in a
 * small per-weed canvas and blitted in a single SPI transfer: the swaying
 * animation never leaves trails and never flickers.
 */
class AquariumManager {
   public:
    static void begin();
    static void update();
    static void pause(bool paused);
    static bool isActive();

    static AquariumConfig defaults();
    static const AquariumConfig& config();
    static bool applyConfig(const AquariumConfig& cfg);
    static const char* lastError();

    static void loadConfig();
    static bool saveConfig();
};
