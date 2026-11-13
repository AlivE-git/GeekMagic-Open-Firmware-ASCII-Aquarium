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

#include "aquarium/AquariumManager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>

#include <Logger.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "display/DisplayManager.h"

namespace {

constexpr const char* CONFIG_PATH = "/aquarium.json";

constexpr uint16_t COLOR_WATER = LCD_BLACK;

constexpr int16_t BASE_MARGIN = 4;          // gap between the bottom bracket and the screen edge
constexpr uint32_t FRAME_INTERVAL_MS = 33;  // ~30 fps
constexpr float FRAME_SECONDS = static_cast<float>(FRAME_INTERVAL_MS) / 1000.0F;

constexpr uint8_t SWAY_STEPS = 64;
constexpr int16_t SINE_FULL = 127;

constexpr int16_t MAX_ARC_HEIGHT = 64;
constexpr size_t MAX_CANVAS_PIXELS = 4096;       // 8 KB of RGB565 for the seaweed
constexpr size_t MAX_FISH_CANVAS_PIXELS = 6144;  // 12 KB of RGB565 for one fish

// sin(2*pi*i/64) scaled to -127..127: no floating point in the animation loop.
constexpr int8_t SIN_LUT[SWAY_STEPS] = {
    0,    12,   25,   37,   49,   60,   71,   81,  90,  98,  106,  112,  117,  122,  125,  126,
    127,  126,  125,  122,  117,  112,  106,  98,  90,  81,  71,   60,   49,   37,   25,   12,
    0,    -12,  -25,  -37,  -49,  -60,  -71,  -81, -90, -98, -106, -112, -117, -122, -125, -126,
    -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71,  -60,  -49,  -37,  -25,  -12,
};

// Fish are drawn from the same 5x7 pixel glyphs the scene is made of.
enum Glyph : uint8_t {
    GLYPH_HEAD_RIGHT = 0,
    GLYPH_HEAD_LEFT,
    GLYPH_OPEN,
    GLYPH_CLOSE,
    GLYPH_EYE,
    GLYPH_CURLY_OPEN,
    GLYPH_CURLY_CLOSE,
    GLYPH_COUNT
};

constexpr int16_t GLYPH_W = 5;
constexpr int16_t GLYPH_H = 7;
// A character is four columns wide, as in monospace text: the glyphs of one fish sit close together
// instead of being spread out, so the sprite keeps the proportions of the ASCII art it stands for.
constexpr int16_t CELL_W = 4;

// Most significant bit is the leftmost column.
constexpr uint8_t GLYPHS[GLYPH_COUNT][GLYPH_H] = {
    {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10},  // >
    {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01},  // <
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},  // (
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},  // )
    // {0x0C, 0x12, 0x0C, 0x00, 0x00, 0x00, 0x00},  // eye
    {0x04, 0x0A, 0x04, 0x00, 0x00, 0x00, 0x00},  // eye
    // {0x04, 0x08, 0x08, 0x10, 0x08, 0x08, 0x04},  // {
    {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02},  // {
    // {0x04, 0x02, 0x02, 0x01, 0x02, 0x02, 0x04},  // }
    {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08},  // }
};

// "><>", "><))>", "><)))>", the bent "><((>" and the tailless ">)))>" variants, each with an eye
// before the mouth, then the same four bodies again with curly braces instead of round ones.
constexpr uint8_t FISH_BODY_FRY[] = {GLYPH_HEAD_RIGHT, GLYPH_HEAD_LEFT, GLYPH_EYE, GLYPH_HEAD_RIGHT};
constexpr uint8_t FISH_BODY_SHORT[] = {GLYPH_HEAD_RIGHT, GLYPH_HEAD_LEFT, GLYPH_CLOSE,
                                       GLYPH_CLOSE,      GLYPH_EYE,       GLYPH_HEAD_RIGHT};
constexpr uint8_t FISH_BODY_LONG[] = {GLYPH_HEAD_RIGHT, GLYPH_HEAD_LEFT, GLYPH_CLOSE,     GLYPH_CLOSE,
                                      GLYPH_CLOSE,      GLYPH_EYE,       GLYPH_HEAD_RIGHT};
constexpr uint8_t FISH_BODY_BENT[] = {GLYPH_HEAD_RIGHT, GLYPH_HEAD_LEFT, GLYPH_OPEN,
                                      GLYPH_OPEN,       GLYPH_EYE,       GLYPH_HEAD_RIGHT};
constexpr uint8_t FISH_BODY_PLAIN[] = {GLYPH_HEAD_RIGHT, GLYPH_CLOSE, GLYPH_CLOSE,
                                       GLYPH_CLOSE,      GLYPH_EYE,   GLYPH_HEAD_RIGHT};
constexpr uint8_t FISH_BODY_SHORT_CURLY[] = {GLYPH_HEAD_RIGHT,  GLYPH_HEAD_LEFT, GLYPH_CURLY_CLOSE,
                                             GLYPH_CURLY_CLOSE, GLYPH_EYE,       GLYPH_HEAD_RIGHT};
constexpr uint8_t FISH_BODY_LONG_CURLY[] = {GLYPH_HEAD_RIGHT,  GLYPH_HEAD_LEFT, GLYPH_CURLY_CLOSE, GLYPH_CURLY_CLOSE,
                                            GLYPH_CURLY_CLOSE, GLYPH_EYE,       GLYPH_HEAD_RIGHT};
constexpr uint8_t FISH_BODY_BENT_CURLY[] = {GLYPH_HEAD_RIGHT, GLYPH_HEAD_LEFT, GLYPH_CURLY_OPEN,
                                            GLYPH_CURLY_OPEN, GLYPH_EYE,       GLYPH_HEAD_RIGHT};
constexpr uint8_t FISH_BODY_PLAIN_CURLY[] = {GLYPH_HEAD_RIGHT,  GLYPH_CURLY_CLOSE, GLYPH_CURLY_CLOSE,
                                             GLYPH_CURLY_CLOSE, GLYPH_EYE,         GLYPH_HEAD_RIGHT};

constexpr int16_t FISH_MAX_CELLS = 7;
constexpr int16_t FISH_MAX_SCALE = 3;
constexpr int16_t FISH_MAX_STEP = 12;            // pixels a fish may travel in a single frame
constexpr int16_t FISH_MAX_BOB = 6;              // extra rows the vertical bob may add
constexpr int16_t FISH_WAVE_AMPLITUDE = 1;       // glyph pixels the body wave lifts each glyph
constexpr int16_t FISH_WAVE_SLOT_STEP = 8;       // phase delay between neighbouring glyphs
constexpr int16_t FISH_WAVE_TRAVEL = 40;         // pixels of travel per full bend of the body
constexpr int16_t FISH_BOB_TRAVEL = 73;          // pixels of travel per bob, deliberately off the wave period
constexpr int16_t FISH_EAT_REACH = 6;            // how close a fish must be to a weed to nibble it
constexpr int16_t FISH_EAT_MS = 700;             // how long the fish hovers over the weed
constexpr float FISH_DIVE_FACTOR = 1.5F;         // dive speed as a multiple of the swim speed
constexpr uint32_t FISH_EAT_MAX_MS = 4000;       // longest dive before the bite lands anyway
constexpr float FISH_ROAM_FACTOR = 0.7F;         // drifting up or down, as a multiple of the swim speed
constexpr int16_t FISH_ROAM_MIN_MS = 1800;       // shortest time a fish keeps one depth
constexpr int16_t FISH_ROAM_MAX_MS = 5500;       // longest time a fish keeps one depth
constexpr uint32_t FISH_EAT_COOLDOWN_MS = 9000;  // pause before the same fish may nibble again
constexpr int16_t FISH_TURN_MS = 260;            // how long the turn animation lasts
constexpr uint16_t FISH_TURN_SQUASH = 200;       // thinnest the fish gets mid-turn, per 1000
constexpr int16_t FISH_IMPULSE_MIN_MS = 2500;    // shortest pause between a speed change and a turn
constexpr int16_t FISH_IMPULSE_MAX_MS = 8000;    // longest pause
constexpr int16_t FISH_MARGIN = 8;               // how far outside the screen a fish starts and ends
constexpr int16_t WATER_TOP = 2;                 // the fish stay above the seaweed

constexpr uint8_t BUBBLE_MAX = 16;         // bubbles alive at the same time
constexpr int16_t BUBBLE_SIZE = 3;         // one bubble is a 3x3 block
constexpr int16_t BUBBLE_SWAY = 2;         // how far a bubble drifts sideways
constexpr uint16_t BUBBLE_COLOR = 0xFFFF;  // white reads as air on the black water

/// A hollow ring: the three rows of one bubble, low bit on the right.
constexpr uint8_t BUBBLE_ROWS[BUBBLE_SIZE] = {0x02, 0x05, 0x02};

constexpr uint16_t FISH_COLORS[] = {0xFD20, 0x07FF, 0xFFE0, 0xF81F, 0xFFFF, 0xFA20};
constexpr uint8_t FISH_COLOR_COUNT = sizeof(FISH_COLORS) / sizeof(FISH_COLORS[0]);

/**
 * @brief Where one fish sprite sits on screen and how it is bent at that moment
 */
struct FishDraw {
    int16_t originX;  // top-left corner of glyph slot 0
    int16_t originY;
    int16_t cellW;    // distance between two glyph slots
    int16_t blockW;   // width of one glyph pixel, squeezed while turning
    uint16_t squash;  // 1000 while the fish is not turning
    uint8_t wavePhase;
    uint8_t facingRight;
};

struct Fish {
    float x;  // top-left corner, pixels
    float y;
    float vx;     // pixels per frame, sign carries the direction
    float baseY;  // vertical centre the bob oscillates around
    uint16_t color;
    const uint8_t* body;  // glyph sequence of the species
    uint8_t scale;        // 1..3, one glyph pixel becomes scale x scale screen pixels
    uint8_t glyphCount;
    int8_t bobAmp;
    uint8_t bobPhase;
    uint8_t waveOffset;     // keeps neighbouring fish out of step with each other
    uint32_t nextEventMs;   // when the fish next changes speed or turns around
    uint32_t turnEndMs;     // 0 while not turning, otherwise when the flip animation ends
    uint8_t turnPending;    // 1 until the direction has been swapped at the thinnest point
    uint32_t eatStartMs;    // when the dive onto the weed started
    uint32_t eatEndMs;      // 0 while diving, otherwise when the hover ends and the bite lands
    uint32_t nextEatMs;     // pause before this fish may stop at a weed again
    int8_t eatWeed;         // index of the weed being eaten, -1 when none
    float eatTargetY;       // vertical position the fish settles into while nibbling
    float wanderY;          // depth the fish is drifting towards
    uint32_t nextWanderMs;  // when it picks a new depth
    uint32_t nextBubbleMs;  // when it may let a bubble go next

    // The last sprite that went to the panel, so the next frame can rub out exactly those pixels
    // and leave every other fish alone.
    uint8_t drawn;        // 0 until this fish has painted itself once
    FishDraw drawnState;  // the pose that is on the panel right now
};

void clearFishRect(const Fish& fish);

/**
 * @brief Width of one sprite in pixels
 *
 * A mirrored glyph reaches into the fifth column while a cell is only CELL_W columns wide, so the
 * last cell is as wide as a glyph. Without that the rightmost column of a left-swimming fish would
 * stick out of the rect that gets erased, leaving a dotted trail behind it.
 */
int16_t fishSpriteWidth(const Fish& fish) {
    return static_cast<int16_t>(((static_cast<int16_t>(fish.glyphCount) - 1) * CELL_W + GLYPH_W) * fish.scale);
}

AquariumConfig s_cfg{};
bool s_cfgReady = false;

uint8_t s_eaten[AQUARIUM_MAX_WEEDS];      // brackets bitten off each weed
uint32_t s_regrowAt[AQUARIUM_MAX_WEEDS];  // when the next bracket of that weed comes back

uint16_t* s_canvas = nullptr;  // seaweed canvas, allocated for the largest scene seen so far
size_t s_canvasPixels = 0;
int16_t s_canvasW = 0;
int16_t s_canvasH = 0;
int16_t s_arcOffset[MAX_ARC_HEIGHT];

Fish s_fish[AQUARIUM_MAX_FISH];
uint16_t* s_fishCanvas = nullptr;  // one fish at a time, sized for the largest configured fish
size_t s_fishCanvasPixels = 0;
int16_t s_fishCanvasW = 0;
int16_t s_fishCanvasH = 0;
bool s_fishReady = false;
int16_t s_fishBand = 0;

bool s_active = false;
bool s_paused = false;
bool s_needsRedraw = false;
uint32_t s_lastFrameMs = 0;

char s_lastError[72] = "";

bool fail(const char* message) {
    std::snprintf(s_lastError, sizeof(s_lastError), "%s", message);

    return false;
}

/**
 * @brief Precompute the horizontal offset of every row of a bracket
 *
 * The arc follows a parabola close enough to a sine: the tips sit at +bulge and
 * the middle at -bulge, which is what makes it look like "(" or ")".
 */
void rebuildArcShape(const AquariumConfig& cfg) {
    const int16_t span = cfg.arcHeight - 1;
    const int32_t denom = static_cast<int32_t>(span) * span;

    for (int16_t row = 0; row < cfg.arcHeight && row < MAX_ARC_HEIGHT; ++row) {
        const int32_t num = 4 * cfg.arcBulge * row * (span - row);
        const int32_t lean = (num + (denom / 2)) / denom;
        s_arcOffset[row] = static_cast<int16_t>(cfg.arcBulge - (2 * lean));
    }
}

/**
 * @brief Derive the seaweed canvas size from the configuration
 *
 * @return false when the requested scene does not fit in RAM
 */
bool computeCanvas(const AquariumConfig& cfg, int16_t& width, int16_t& height) {
    width = static_cast<int16_t>((2 * (cfg.arcBulge + cfg.swayMax + cfg.stagger)) + cfg.arcStroke + 2);

    int16_t tallest = cfg.arcHeight;
    for (uint8_t i = 0; i < cfg.weedCount; ++i) {
        const int16_t weedHeight = ((cfg.weeds[i].brackets - 1) * cfg.arcOffset) + cfg.arcHeight;
        if (weedHeight > tallest) {
            tallest = weedHeight;
        }
    }
    height = tallest;

    if (width <= 0 || height <= 0 || height > MAX_ARC_HEIGHT * 4) {
        return false;
    }

    return (static_cast<size_t>(width) * static_cast<size_t>(height)) <= MAX_CANVAS_PIXELS;
}

/**
 * @brief Draw one bracket into the seaweed canvas
 *
 * @param baseY Top row of the bracket inside the canvas
 * @param direction +1 for "(" (bulges left), -1 for ")" (bulges right)
 * @param shift Horizontal shift in pixels
 */
void drawArc(int16_t baseY, int16_t direction, int16_t shift) {
    const int16_t centerX = (s_canvasW / 2) + shift;

    for (int16_t row = 0; row < s_cfg.arcHeight && row < MAX_ARC_HEIGHT; ++row) {
        const int16_t y = baseY + row;
        if (y < 0 || y >= s_canvasH) {
            continue;
        }

        const int16_t topX = centerX + (direction * s_arcOffset[row]);
        for (int16_t px = 0; px < s_cfg.arcStroke; ++px) {
            const int16_t x = topX + px;
            if (x < 0 || x >= s_canvasW) {
                continue;
            }

            s_canvas[(static_cast<int32_t>(y) * s_canvasW) + x] = s_cfg.color;
        }
    }
}

/**
 * @brief Compose one weed in the canvas and blit it to the panel
 *
 * The canvas is opaque, so the blit also erases the previous frame: no separate
 * erase pass and no flicker.
 */
void renderWeed(const AquariumWeed& weed, uint8_t levels, uint8_t phase) {
    for (int32_t i = 0; i < static_cast<int32_t>(s_canvasW) * s_canvasH; ++i) {
        s_canvas[i] = COLOR_WATER;
    }

    // A bitten weed is drawn with fewer levels, so it is shorter on screen while the canvas and the
    // fish band keep the height the plant will grow back to.
    const int16_t weedHeight = ((levels - 1) * s_cfg.arcOffset) + s_cfg.arcHeight;
    // Canvas rows grow downwards, so this is the top row of the topmost bracket.
    const int16_t plantTopY = s_canvasH - weedHeight;
    // Level 0 is the bracket rooted at the bottom of the plant, the tip sways the most.
    const int16_t swaySpan = (levels > 1) ? (levels - 1) : 1;

    for (uint8_t level = 0; level < levels; ++level) {
        const int16_t amplitude = (s_cfg.swayMax * level) / swaySpan;
        const int8_t wave = SIN_LUT[(phase + (level * s_cfg.swayPhaseStep)) & (SWAY_STEPS - 1)];
        const int16_t sway = (amplitude * wave) / SINE_FULL;

        // Every bracket above the base steps sideways, alternating left and right.
        const int16_t lateral = (level == 0) ? 0 : (((level % 2) == 1) ? -s_cfg.stagger : s_cfg.stagger);
        // The base is always "(", the ones above alternate: ( ) (
        const int16_t direction = ((level % 2) == 0) ? 1 : -1;

        const int16_t baseY = plantTopY + ((levels - 1 - level) * s_cfg.arcOffset);

        drawArc(baseY, direction, sway + lateral);
    }

    DisplayManager::getGfx()->draw16bitRGBBitmap(weed.axisX - (s_canvasW / 2),
                                                 DisplayManager::getGfx()->height() - BASE_MARGIN - s_canvasH, s_canvas,
                                                 s_canvasW, s_canvasH);
}

/**
 * @brief Give one fish its own size, color, speed and depth
 *
 * Everything rolled here is kept for the whole time the fish is on screen: the
 * fish only gets new attributes after it has left the display. Fish are no
 * longer kept apart, so they may swim into each other and cross.
 */
void spawnFish(Fish& fish, uint8_t index, int16_t band) {
    const int16_t screenW = DisplayManager::getGfx()->width();
    const int16_t scaleLow = (s_cfg.fishScaleMin < s_cfg.fishScaleMax) ? s_cfg.fishScaleMin : s_cfg.fishScaleMax;
    const int16_t scaleHigh = (s_cfg.fishScaleMax > s_cfg.fishScaleMin) ? s_cfg.fishScaleMax : s_cfg.fishScaleMin;
    const int16_t speedLow = (s_cfg.fishSpeedMin < s_cfg.fishSpeedMax) ? s_cfg.fishSpeedMin : s_cfg.fishSpeedMax;
    const int16_t speedHigh = (s_cfg.fishSpeedMax > s_cfg.fishSpeedMin) ? s_cfg.fishSpeedMax : s_cfg.fishSpeedMin;
    const int16_t bobLow = (s_cfg.fishBobMin < s_cfg.fishBobMax) ? s_cfg.fishBobMin : s_cfg.fishBobMax;
    const int16_t bobHigh = (s_cfg.fishBobMax > s_cfg.fishBobMin) ? s_cfg.fishBobMax : s_cfg.fishBobMin;

    fish.scale = static_cast<uint8_t>(random(scaleLow, scaleHigh + 1));

    // Every species is one glyph sequence plus its length; nothing else about a fish depends on it.
    struct Species {
        const uint8_t* body;
        uint8_t length;
    };

    const Species species[] = {
        {FISH_BODY_FRY, static_cast<uint8_t>(sizeof(FISH_BODY_FRY))},
        {FISH_BODY_SHORT, static_cast<uint8_t>(sizeof(FISH_BODY_SHORT))},
        {FISH_BODY_LONG, static_cast<uint8_t>(sizeof(FISH_BODY_LONG))},
        {FISH_BODY_BENT, static_cast<uint8_t>(sizeof(FISH_BODY_BENT))},
        {FISH_BODY_PLAIN, static_cast<uint8_t>(sizeof(FISH_BODY_PLAIN))},
        {FISH_BODY_SHORT_CURLY, static_cast<uint8_t>(sizeof(FISH_BODY_SHORT_CURLY))},
        {FISH_BODY_LONG_CURLY, static_cast<uint8_t>(sizeof(FISH_BODY_LONG_CURLY))},
        {FISH_BODY_BENT_CURLY, static_cast<uint8_t>(sizeof(FISH_BODY_BENT_CURLY))},
        {FISH_BODY_PLAIN_CURLY, static_cast<uint8_t>(sizeof(FISH_BODY_PLAIN_CURLY))},
    };
    const uint8_t speciesCount = static_cast<uint8_t>(sizeof(species) / sizeof(species[0]));
    const Species& picked = species[random(0, speciesCount)];

    fish.body = picked.body;
    fish.glyphCount = picked.length;

    fish.color = FISH_COLORS[random(0, FISH_COLOR_COUNT)];

    const int16_t width = fishSpriteWidth(fish);
    const int16_t height = GLYPH_H * fish.scale;

    const int16_t speed = static_cast<int16_t>(random(speedLow, speedHigh + 1));  // pixels per second
    const float perFrame = static_cast<float>(speed) * FRAME_SECONDS;

    const bool swimsRight = (random(0, 2) == 0);
    fish.vx = swimsRight ? perFrame : -perFrame;

    // Start outside the screen and swim in from there.
    fish.x = swimsRight ? -static_cast<float>(width + FISH_MARGIN + random(0, 24))
                        : static_cast<float>(screenW + FISH_MARGIN + random(0, 24));

    const int16_t wavePad = static_cast<int16_t>(FISH_WAVE_AMPLITUDE * 2 * fish.scale);
    const int16_t maxBase = static_cast<int16_t>(WATER_TOP + band - height - wavePad - 1);
    const int16_t room = (maxBase > WATER_TOP) ? static_cast<int16_t>(maxBase - WATER_TOP) : 0;

    float baseY = 0.0F;
    if (s_cfg.fishSwim == FishSwim::Lanes) {
        // Every fish keeps its own horizontal lane, so the tank never has two fish in one row.
        const int16_t lane = (s_cfg.fishCount > 0) ? static_cast<int16_t>(band / s_cfg.fishCount) : band;
        const int16_t laneTop = static_cast<int16_t>(WATER_TOP + (index * lane));
        const int16_t slack = (lane > height) ? static_cast<int16_t>(lane - height) : 0;
        baseY = static_cast<float>(laneTop + random(0, slack + 1));
    } else {
        // Free swimming: a fish comes in at whatever depth it likes.
        baseY = static_cast<float>(WATER_TOP + random(0, room + 1));
    }
    if (baseY > static_cast<float>(maxBase)) {
        baseY = static_cast<float>((maxBase > WATER_TOP) ? maxBase : WATER_TOP);
    }
    if (baseY < static_cast<float>(WATER_TOP)) {
        baseY = static_cast<float>(WATER_TOP);
    }

    fish.baseY = baseY;
    fish.eatTargetY = baseY;
    fish.y = baseY;
    fish.bobAmp = static_cast<int8_t>(random(bobLow, bobHigh + 1));
    fish.bobPhase = static_cast<uint8_t>(random(0, SWAY_STEPS));
    fish.waveOffset = static_cast<uint8_t>(random(0, SWAY_STEPS));
    fish.nextEventMs = millis() + static_cast<uint32_t>(random(FISH_IMPULSE_MIN_MS, FISH_IMPULSE_MAX_MS + 1));
    fish.turnEndMs = 0;
    fish.turnPending = 0;
    fish.eatStartMs = 0;
    fish.eatEndMs = 0;
    fish.nextEatMs = 0;
    fish.eatWeed = -1;
    fish.wanderY = baseY;
    fish.nextWanderMs = millis() + static_cast<uint32_t>(random(FISH_ROAM_MIN_MS, FISH_ROAM_MAX_MS + 1));
    fish.nextBubbleMs = millis() + static_cast<uint32_t>(random(0, s_cfg.bubbleEveryMs));
    fish.drawn = 0;
}

/**
 * @brief Let a fish change its speed or turn around while it is swimming
 *
 * Both live in vx: its length is the speed and its sign the direction, so a turn is a sign flip
 * that keeps the current pace and a new pace is applied in the direction the fish already has.
 */
void applyImpulse(Fish& fish) {
    int16_t low = (s_cfg.fishSpeedMin < s_cfg.fishSpeedMax) ? s_cfg.fishSpeedMin : s_cfg.fishSpeedMax;
    int16_t high = (s_cfg.fishSpeedMax > s_cfg.fishSpeedMin) ? s_cfg.fishSpeedMax : s_cfg.fishSpeedMin;

    if (random(0, 2) == 0) {
        if (s_cfg.fishTurnAnim) {
            // The direction itself is swapped halfway through the flip, not here.
            fish.turnEndMs = millis() + FISH_TURN_MS;
            fish.turnPending = 1;
        } else {
            fish.vx = -fish.vx;
        }
    }

    const float perFrame = static_cast<float>(random(low, high + 1)) * FRAME_SECONDS;
    fish.vx = (fish.vx < 0.0F) ? -perFrame : perFrame;

    fish.nextEventMs = millis() + static_cast<uint32_t>(random(FISH_IMPULSE_MIN_MS, FISH_IMPULSE_MAX_MS + 1));
}

/**
 * @brief Recompute the water band, spawn the missing fish and keep the rest inside it
 */
void layoutFish(uint8_t previousCount) {
    const int16_t weedTop = DisplayManager::getGfx()->height() - BASE_MARGIN - s_canvasH;
    s_fishBand = static_cast<int16_t>(weedTop - WATER_TOP);

    if (s_fishBand < GLYPH_H) {
        return;
    }

    for (uint8_t i = 0; i < s_cfg.fishCount; ++i) {
        if (!s_fishReady || i >= previousCount) {
            spawnFish(s_fish[i], i, s_fishBand);
            continue;
        }

        const int16_t height = GLYPH_H * s_fish[i].scale;
        const int16_t wavePad = static_cast<int16_t>(FISH_WAVE_AMPLITUDE * 2 * s_fish[i].scale);
        const int16_t maxBase = static_cast<int16_t>(WATER_TOP + s_fishBand - height - wavePad - 1);
        const float deepest = (maxBase > WATER_TOP) ? static_cast<float>(maxBase) : static_cast<float>(WATER_TOP);
        if (s_fish[i].baseY > deepest) {
            s_fish[i].baseY = deepest;
        }
        if (s_fish[i].wanderY > deepest) {
            s_fish[i].wanderY = deepest;
        }
    }

    // Fish that were just removed have to take their last sprite with them.
    for (uint8_t i = s_cfg.fishCount; i < previousCount && i < AQUARIUM_MAX_FISH; ++i) {
        clearFishRect(s_fish[i]);
    }

    s_fishReady = true;
}

/**
 * @brief Bring a bitten bracket back once its timer has run out
 */
void regrowWeeds() {
    for (uint8_t i = 0; i < s_cfg.weedCount; ++i) {
        if (s_eaten[i] == 0 || static_cast<int32_t>(millis() - s_regrowAt[i]) < 0) {
            continue;
        }

        --s_eaten[i];
        s_regrowAt[i] = millis() + (static_cast<uint32_t>(s_cfg.weedGrowS) * 1000UL);
    }
}

/**
 * @brief Is another fish already on its way to this weed?
 *
 * The plant counts as taken from the moment a fish starts its dive, not when the bite lands, so two
 * fish never end up hovering over the same plant - the second would be eating nothing.
 */
bool weedIsTaken(const Fish& fish, uint8_t weedIndex) {
    for (uint8_t i = 0; i < s_cfg.fishCount; ++i) {
        const Fish& other = s_fish[i];
        if (&other != &fish && other.eatWeed == static_cast<int8_t>(weedIndex)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Stop a fish over a tall weed so that it can nibble one bracket off
 */
void tryStartEating(Fish& fish) {
    if (s_cfg.weedCount == 0 || !s_cfg.weedEat || s_cfg.weedGrowS == 0) {
        return;
    }

    // Only the tallest plants are worth eating: while one of them grows back there is nothing to eat.
    uint8_t tallest = 0;
    for (uint8_t i = 0; i < s_cfg.weedCount; ++i) {
        if (s_cfg.weeds[i].brackets > tallest) {
            tallest = s_cfg.weeds[i].brackets;
        }
    }

    const int16_t screenH = DisplayManager::getGfx()->height();

    if (s_cfg.fishSwim == FishSwim::Lanes) {
        // Only the lane right above the plants can reach them.
        const int16_t laneCount = (s_cfg.fishCount > 0) ? static_cast<int16_t>(s_cfg.fishCount) : 1;
        const int16_t laneHeight = static_cast<int16_t>(s_fishBand / laneCount);
        const int16_t lowestLaneTop = static_cast<int16_t>(WATER_TOP + ((laneCount - 1) * laneHeight));
        if (fish.baseY + 0.5F < static_cast<float>(lowestLaneTop)) {
            return;
        }
    } else if (fish.baseY + 0.5F < static_cast<float>(WATER_TOP + (s_fishBand / 2))) {
        // With free swimming the depth is the only thing that counts: a fish near the surface never
        // dives, it is the ones cruising low over the plants that stop to nibble.
        return;
    }

    // The mouth has to reach the plant, not the middle of the body: the head sits on the leading end
    // of the sprite, which is the right edge going right and the left edge going left.
    const int16_t scale = fish.scale;
    const int16_t headOffset =
        (fish.vx > 0.0F) ? static_cast<int16_t>(((static_cast<int16_t>(fish.glyphCount) - 1) * CELL_W * scale) +
                                                ((GLYPH_W * scale) / 2))
                         : static_cast<int16_t>((GLYPH_W * scale) / 2);
    const float headX = fish.x + static_cast<float>(headOffset);

    for (uint8_t i = 0; i < s_cfg.weedCount; ++i) {
        // Only a whole plant with a bracket to spare is worth a bite, and only one fish at a time.
        if (s_cfg.weeds[i].brackets != tallest || s_eaten[i] != 0) {
            continue;
        }
        if (weedIsTaken(fish, i)) {
            continue;
        }

        const int16_t plantHeight =
            static_cast<int16_t>(((s_cfg.weeds[i].brackets - 1) * s_cfg.arcOffset) + s_cfg.arcHeight);
        const int16_t plantTop = static_cast<int16_t>(screenH - BASE_MARGIN - plantHeight);

        if (fabsf(headX - static_cast<float>(s_cfg.weeds[i].axisX)) > static_cast<float>(FISH_EAT_REACH)) {
            continue;
        }
        if (random(0, 100) >= static_cast<long>(s_cfg.fishEatChance)) {
            fish.nextEatMs = millis() + 1500;  // swam past this plant without stopping
            return;
        }

        // Dive onto the tip so the bite happens right at the plant. The dive itself runs at the
        // fish's own pace, so the hover starts once it has actually arrived.
        fish.eatTargetY = static_cast<float>(plantTop) - static_cast<float>(GLYPH_H * fish.scale);
        fish.eatStartMs = millis();
        fish.eatEndMs = 0;
        fish.nextEatMs = millis() + FISH_EAT_COOLDOWN_MS;
        fish.eatWeed = static_cast<int8_t>(i);

        return;
    }
}

/**
 * @brief Take one bracket off the weed the fish has been hovering over
 */
void finishBite(Fish& fish) {
    const int8_t index = fish.eatWeed;
    if (s_cfg.weedEat && index >= 0 && index < static_cast<int8_t>(s_cfg.weedCount)) {
        const uint8_t weedIndex = static_cast<uint8_t>(index);
        if (s_eaten[weedIndex] == 0) {
            s_eaten[weedIndex] = 1;
            s_regrowAt[weedIndex] = millis() + (static_cast<uint32_t>(s_cfg.weedGrowS) * 1000UL);
        }
    }

    fish.eatEndMs = 0;
    fish.eatWeed = -1;
}

/**
 * @brief Screen rectangle one pose of a fish covers
 */
void spriteRect(const Fish& fish, const FishDraw& draw, int16_t& left, int16_t& top, int16_t& w, int16_t& h) {
    const int16_t waveAmp = static_cast<int16_t>(FISH_WAVE_AMPLITUDE * fish.scale);
    left = draw.originX;
    top = static_cast<int16_t>(draw.originY - waveAmp);
    w = fishSpriteWidth(fish);
    h = static_cast<int16_t>((GLYPH_H * fish.scale) + (2 * waveAmp));
}

/**
 * @brief Rub out the rectangle the fish last occupied
 */
void clearFishRect(const Fish& fish) {
    if (fish.drawn == 0) {
        return;
    }

    int16_t left = 0;
    int16_t top = 0;
    int16_t w = 0;
    int16_t h = 0;
    spriteRect(fish, fish.drawnState, left, top, w, h);
    DisplayManager::getGfx()->fillRect(left, top, w, h, COLOR_WATER);
}

/**
 * @brief Draw one fish pose into a buffer, clipped to that buffer
 *
 * The buffer holds a rectangle of the panel, so the same routine paints the fish being moved and any
 * neighbour that happens to live inside that rectangle as well.
 */
void paintFishInto(uint16_t* buffer, int16_t left, int16_t top, int16_t rectW, int16_t rectH, const Fish& fish,
                   const FishDraw& draw, uint16_t color) {
    const int16_t waveAmp = static_cast<int16_t>(FISH_WAVE_AMPLITUDE * fish.scale);

    for (uint8_t slot = 0; slot < fish.glyphCount; ++slot) {
        const uint8_t glyph = draw.facingRight ? fish.body[slot] : fish.body[fish.glyphCount - 1 - slot];

        int16_t shift = 0;
        if (waveAmp > 0) {
            const int16_t index = static_cast<int16_t>(
                (draw.wavePhase + fish.waveOffset - (slot * FISH_WAVE_SLOT_STEP)) & (SWAY_STEPS - 1));
            shift = static_cast<int16_t>((SIN_LUT[index] * waveAmp) / SINE_FULL);
        }

        const int16_t cellX = static_cast<int16_t>(draw.originX - left + (slot * draw.cellW));
        const int16_t cellY = static_cast<int16_t>(draw.originY - top + shift);

        for (int16_t row = 0; row < GLYPH_H; ++row) {
            const uint8_t bits = GLYPHS[glyph][row];
            if (bits == 0) {
                continue;
            }

            const int16_t blockY = static_cast<int16_t>(cellY + (row * fish.scale));

            for (int16_t col = 0; col < GLYPH_W; ++col) {
                if (((bits >> (GLYPH_W - 1 - col)) & 1) == 0) {
                    continue;
                }

                const int16_t blockX = static_cast<int16_t>(
                    cellX + (((draw.facingRight ? col : (GLYPH_W - 1 - col)) * fish.scale * draw.squash) / 1000));

                for (int16_t sy = 0; sy < fish.scale; ++sy) {
                    const int16_t cy = static_cast<int16_t>(blockY + sy);
                    if (cy < 0 || cy >= rectH) {
                        continue;
                    }

                    for (int16_t sx = 0; sx < draw.blockW; ++sx) {
                        const int16_t cx = static_cast<int16_t>(blockX + sx);
                        if (cx < 0 || cx >= rectW) {
                            continue;
                        }

                        buffer[(static_cast<int32_t>(cy) * rectW) + cx] = color;
                    }
                }
            }
        }
    }
}

/**
 * @brief Move one fish, respawn it once it has left the screen, and redraw it
 */
void renderFish(Fish& fish, uint8_t index) {
    const int16_t screenW = DisplayManager::getGfx()->width();
    const int16_t width = fishSpriteWidth(fish);

    // The dive onto the plant keeps the pace the fish swims at, instead of snapping to the tip: a
    // slow fish takes its time, a fast one dips quickly. The hover starts when it gets there.
    if (fish.eatWeed >= 0) {
        const float delta = fish.eatTargetY - fish.baseY;
        const float step = fabsf(fish.vx) * FISH_DIVE_FACTOR;

        if (fabsf(delta) <= step) {
            fish.baseY = fish.eatTargetY;
            if (fish.eatEndMs == 0) {
                fish.eatEndMs = millis() + FISH_EAT_MS;
            }
        } else {
            fish.baseY += (delta > 0.0F) ? step : -step;
        }

        const bool arrived = (fish.eatEndMs != 0);
        if ((arrived && static_cast<int32_t>(millis() - fish.eatEndMs) >= 0) ||
            (!arrived && static_cast<int32_t>(millis() - fish.eatStartMs) >= static_cast<int32_t>(FISH_EAT_MAX_MS))) {
            finishBite(fish);
        }
    } else {
        if (static_cast<int32_t>(millis() - fish.nextEventMs) >= 0) {
            applyImpulse(fish);
        }
        if (static_cast<int32_t>(millis() - fish.nextEatMs) >= 0) {
            tryStartEating(fish);
        }
    }

    const bool nibbling = (fish.eatWeed >= 0);
    const int16_t height = GLYPH_H * fish.scale;

    // The wave lifts single glyphs up and down, so the sprite is framed by the largest shift.
    const int16_t waveAmp = FISH_WAVE_AMPLITUDE * fish.scale;

    const int16_t maxBase = static_cast<int16_t>(WATER_TOP + s_fishBand - height - (2 * waveAmp) - 1);

    if (!nibbling) {
        fish.x += fish.vx;

        // Free swimming: every few seconds the fish picks a new depth and drifts towards it at its own
        // pace, so the tank gets crossed in every direction. In lanes mode it stays in its row.
        if (s_cfg.fishSwim == FishSwim::Free) {
            if (static_cast<int32_t>(millis() - fish.nextWanderMs) >= 0) {
                const int16_t room = (maxBase > WATER_TOP) ? static_cast<int16_t>(maxBase - WATER_TOP) : 0;
                fish.wanderY = static_cast<float>(WATER_TOP + random(0, room + 1));
                fish.nextWanderMs = millis() + static_cast<uint32_t>(random(FISH_ROAM_MIN_MS, FISH_ROAM_MAX_MS + 1));
            }

            const float step = fabsf(fish.vx) * FISH_ROAM_FACTOR;
            const float drift = fish.wanderY - fish.baseY;
            if (fabsf(drift) <= step) {
                fish.baseY = fish.wanderY;
            } else {
                fish.baseY += (drift > 0.0F) ? step : -step;
            }
        }
    }

    // Gone past the edge for good: the next one enters with fresh attributes.
    if ((fish.vx > 0.0F && fish.x > static_cast<float>(screenW + FISH_MARGIN)) ||
        (fish.vx < 0.0F && (fish.x + static_cast<float>(width)) < -static_cast<float>(FISH_MARGIN))) {
        clearFishRect(fish);
        spawnFish(fish, index, s_fishBand);

        return;
    }

    // The bob rides on the distance swum as well, but on its own period, so it neither drifts out of
    // time with the speed nor locks onto the body wave.
    const uint8_t bobIndex =
        static_cast<uint8_t>(((nibbling ? (static_cast<int32_t>(millis()) / 15)
                                        : ((static_cast<int32_t>(lroundf(fish.x)) * SWAY_STEPS) / FISH_BOB_TRAVEL)) +
                              fish.bobPhase) &
                             (SWAY_STEPS - 1));
    const int16_t bob = static_cast<int16_t>((SIN_LUT[bobIndex] * fish.bobAmp) / SINE_FULL);

    int16_t targetY = static_cast<int16_t>(lroundf(fish.baseY)) + bob;
    if (targetY > maxBase) {
        targetY = (maxBase > WATER_TOP) ? maxBase : WATER_TOP;
    }
    if (targetY < WATER_TOP) {
        targetY = WATER_TOP;
    }
    fish.y = static_cast<float>(targetY);

    const int16_t drawX = static_cast<int16_t>(lroundf(fish.x));
    const int16_t drawY = targetY;

    // A turn is animated instead of snapping: the sprite squeezes to a sliver, the direction is
    // swapped at its thinnest point and the fish opens up again already facing the other way.
    uint16_t squash = 1000;
    if (fish.turnEndMs != 0) {
        const int32_t remaining = static_cast<int32_t>(fish.turnEndMs - millis());
        if (remaining <= 0) {
            fish.turnEndMs = 0;
            fish.turnPending = 0;
        } else {
            const int32_t elapsed = FISH_TURN_MS - remaining;
            const int32_t half = FISH_TURN_MS / 2;
            if (elapsed >= half && fish.turnPending != 0) {
                fish.vx = -fish.vx;
                fish.turnPending = 0;
            }

            const int32_t phase = (elapsed < half) ? (half - elapsed) : (elapsed - half);
            squash = static_cast<uint16_t>(FISH_TURN_SQUASH + ((1000 - FISH_TURN_SQUASH) * phase * 2) / FISH_TURN_MS);
        }
    }

    // Squeezing the sprite horizontally reads as the fish turning its side to the viewer.
    const int16_t squashedWidth = static_cast<int16_t>((width * squash) / 1000);

    // Every glyph sits on the body axis and rides up and down with a delay of a few phase steps, so a
    // wave runs from tail to mouth. The phase follows the distance the fish has travelled, which keeps
    // the beat in time with its speed and turns the wave around when the fish changes course. While a
    // fish hovers over a weed it keeps undulating on time alone, since it is not moving.
    const uint8_t wavePhase =
        static_cast<uint8_t>(((nibbling ? (static_cast<int32_t>(millis()) / 12)
                                        : ((static_cast<int32_t>(lroundf(fish.x)) * SWAY_STEPS) / FISH_WAVE_TRAVEL)) +
                              fish.waveOffset) &
                             (SWAY_STEPS - 1));

    FishDraw now{};
    now.originX = static_cast<int16_t>(drawX + ((width - squashedWidth) / 2));
    now.originY = static_cast<int16_t>(drawY - waveAmp);
    now.cellW = static_cast<int16_t>((CELL_W * fish.scale * squash) / 1000);
    now.blockW = static_cast<int16_t>((fish.scale * squash) / 1000);
    if (now.cellW < 1) {
        now.cellW = 1;
    }
    if (now.blockW < 1) {
        now.blockW = 1;
    }
    now.squash = squash;
    now.wavePhase = wavePhase;
    now.facingRight = (fish.vx > 0.0F) ? 1 : 0;

    // The blit covers where the fish is now and where it was last frame, so the old pose disappears in
    // the same operation that draws the new one.
    int16_t left = now.originX;
    int16_t top = static_cast<int16_t>(now.originY - waveAmp);
    int16_t right = static_cast<int16_t>(left + width);
    int16_t bottom = static_cast<int16_t>(top + height + (2 * waveAmp));

    if (fish.drawn != 0) {
        int16_t oldLeft = 0;
        int16_t oldTop = 0;
        int16_t oldW = 0;
        int16_t oldH = 0;
        spriteRect(fish, fish.drawnState, oldLeft, oldTop, oldW, oldH);
        if (oldLeft < left) {
            left = oldLeft;
        }
        if (oldTop < top) {
            top = oldTop;
        }
        if ((oldLeft + oldW) > right) {
            right = static_cast<int16_t>(oldLeft + oldW);
        }
        if ((oldTop + oldH) > bottom) {
            bottom = static_cast<int16_t>(oldTop + oldH);
        }
    }

    int16_t rectW = static_cast<int16_t>(right - left);
    int16_t rectH = static_cast<int16_t>(bottom - top);
    if (rectW <= 0 || rectH <= 0 || s_fishCanvas == nullptr) {
        return;
    }
    if (rectW > s_fishCanvasW) {
        rectW = s_fishCanvasW;
    }
    if (rectH > s_fishCanvasH) {
        rectH = s_fishCanvasH;
    }

    for (int32_t i = 0; i < static_cast<int32_t>(rectW) * rectH; ++i) {
        s_fishCanvas[i] = COLOR_WATER;
    }

    // Every other fish that already stands on the panel is reproduced inside this rectangle first, so
    // the blit below cannot cut a hole through a neighbour it crosses.
    for (uint8_t i = 0; i < s_cfg.fishCount; ++i) {
        const Fish& other = s_fish[i];
        if (&other == &fish || other.drawn == 0) {
            continue;
        }

        int16_t otherLeft = 0;
        int16_t otherTop = 0;
        int16_t otherW = 0;
        int16_t otherH = 0;
        spriteRect(other, other.drawnState, otherLeft, otherTop, otherW, otherH);
        if (otherLeft >= (left + rectW) || (otherLeft + otherW) <= left || otherTop >= (top + rectH) ||
            (otherTop + otherH) <= top) {
            continue;
        }

        paintFishInto(s_fishCanvas, left, top, rectW, rectH, other, other.drawnState, other.color);
    }

    paintFishInto(s_fishCanvas, left, top, rectW, rectH, fish, now, fish.color);

    DisplayManager::getGfx()->draw16bitRGBBitmap(left, top, s_fishCanvas, rectW, rectH);

    fish.drawnState = now;
    fish.drawn = 1;
}

/**
 * @brief One air bubble on its way up
 *
 * A bubble is two pixels of white, so it needs no canvas: the old spot is wiped and the new one is
 * painted, two tiny fills per bubble per frame.
 */
struct Bubble {
    float y;        // row it is floating at
    int16_t axisX;  // column it sways around
    uint8_t phase;  // where it is in its sway cycle
    uint8_t active;
    uint8_t drawn;
    int16_t drawnX;
    int16_t drawnY;
};

Bubble s_bubbles[BUBBLE_MAX];

/**
 * @brief Wipe the pixels a bubble painted on the previous frame
 */
void eraseBubble(Bubble& bubble) {
    if (bubble.drawn == 0) {
        return;
    }

    DisplayManager::getGfx()->fillRect(bubble.drawnX, bubble.drawnY, BUBBLE_SIZE, BUBBLE_SIZE, COLOR_WATER);
    bubble.drawn = 0;
}

/**
 * @brief Float every bubble up, sway it sideways and paint it again
 */
void updateBubbles() {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    const float rise = static_cast<float>(s_cfg.bubbleRise) * FRAME_SECONDS;

    for (uint8_t i = 0; i < BUBBLE_MAX; ++i) {
        Bubble& bubble = s_bubbles[i];
        if (bubble.active == 0) {
            continue;
        }

        eraseBubble(bubble);

        if (!s_cfg.bubbles) {
            bubble.active = 0;

            continue;
        }

        bubble.y -= rise;
        bubble.phase = static_cast<uint8_t>((bubble.phase + 1) & (SWAY_STEPS - 1));

        if (bubble.y < static_cast<float>(WATER_TOP)) {
            bubble.active = 0;

            continue;
        }

        const int16_t sway = static_cast<int16_t>((SIN_LUT[bubble.phase] * BUBBLE_SWAY) / SINE_FULL);
        const int16_t x = static_cast<int16_t>(bubble.axisX + sway);
        const int16_t y = static_cast<int16_t>(lroundf(bubble.y));

        // Row by row: horizontal runs of the ring, so a bubble costs four tiny fills and not nine.
        for (int8_t row = 0; row < BUBBLE_SIZE; ++row) {
            const uint8_t bits = BUBBLE_ROWS[row];
            int8_t col = 0;

            while (col < BUBBLE_SIZE) {
                if ((bits & (0x04 >> col)) == 0) {
                    ++col;

                    continue;
                }

                int8_t run = 1;
                while (col + run < BUBBLE_SIZE && (bits & (0x04 >> (col + run))) != 0) {
                    ++run;
                }

                gfx->fillRect(static_cast<int16_t>(x + col), static_cast<int16_t>(y + row), run, 1, BUBBLE_COLOR);
                col = static_cast<int8_t>(col + run);
            }
        }

        bubble.drawn = 1;
        bubble.drawnX = x;
        bubble.drawnY = y;
    }
}

/**
 * @brief Roll for a bubble at a fish and let one go if the dice agree
 */
void emitBubble(Fish& fish) {
    if (!s_cfg.bubbles || static_cast<int32_t>(millis() - fish.nextBubbleMs) < 0) {
        return;
    }

    fish.nextBubbleMs = millis() + s_cfg.bubbleEveryMs;

    if (random(0, 100) >= static_cast<long>(s_cfg.bubbleChance)) {
        return;
    }

    for (uint8_t i = 0; i < BUBBLE_MAX; ++i) {
        Bubble& bubble = s_bubbles[i];
        if (bubble.active != 0) {
            continue;
        }

        bubble.active = 1;
        bubble.drawn = 0;
        bubble.y = fish.y - static_cast<float>(BUBBLE_SIZE);  // above the body, so it shows at once
        bubble.axisX = static_cast<int16_t>(lroundf(fish.x) + (fishSpriteWidth(fish) / 2));
        bubble.phase = static_cast<uint8_t>(random(0, SWAY_STEPS));

        return;
    }
}

void renderFrame() {
    const uint8_t phase = static_cast<uint8_t>((millis() / s_cfg.swayStepMs) % SWAY_STEPS);

    regrowWeeds();

    // Bubbles go first: their old pixels are wiped before the plants and the fish are painted, so an
    // erased bubble can never leave a hole in either of them.
    updateBubbles();

    for (uint8_t i = 0; i < s_cfg.weedCount; ++i) {
        const uint8_t levels = static_cast<uint8_t>(s_cfg.weeds[i].brackets - s_eaten[i]);
        renderWeed(s_cfg.weeds[i], levels, phase);
    }

    // Fish may cross each other, so each one paints over whatever is already there and then clears the
    // pixels it has left behind: no frame-wide erase pass and never a blank fish on screen.
    if (s_fishBand >= GLYPH_H) {
        for (uint8_t i = 0; i < s_cfg.fishCount; ++i) {
            renderFish(s_fish[i], i);
            emitBubble(s_fish[i]);
        }
    }
}

}  // namespace

AquariumConfig AquariumManager::defaults() {
    AquariumConfig cfg{};
    cfg.arcHeight = 27;
    cfg.arcOffset = 9;
    cfg.arcBulge = 5;
    cfg.arcStroke = 1;
    cfg.stagger = 3;
    cfg.swayMax = 4;
    cfg.swayPhaseStep = 0;
    cfg.swayStepMs = 60;
    cfg.color = LCD_GREEN;
    cfg.fishCount = 3;
    cfg.fishScaleMin = 2;
    cfg.fishScaleMax = 3;
    cfg.fishSpeedMin = 15;
    cfg.fishSpeedMax = 45;
    cfg.fishBobMin = 1;
    cfg.fishBobMax = 3;
    cfg.fishTurnAnim = true;
    cfg.fishSwim = FishSwim::Free;
    cfg.bubbles = true;
    cfg.bubbleChance = 20;
    cfg.bubbleRise = 10;
    cfg.bubbleEveryMs = 1200;
    cfg.weedGrowS = 20;
    cfg.weedEat = true;
    cfg.fishEatChance = 30;
    cfg.weedCount = 2;
    cfg.weeds[0] = {72, 2};
    cfg.weeds[1] = {168, 3};
    cfg.weeds[2] = {120, 2};
    cfg.weeds[3] = {48, 1};
    cfg.weeds[4] = {192, 1};

    return cfg;
}

const AquariumConfig& AquariumManager::config() {
    if (!s_cfgReady) {
        s_cfg = defaults();
        s_cfgReady = true;
    }

    return s_cfg;
}

const char* AquariumManager::lastError() { return s_lastError; }

bool AquariumManager::applyConfig(const AquariumConfig& cfg) {
    const uint8_t previousFishCount = s_cfg.fishCount;
    const FishSwim previousSwim = s_cfg.fishSwim;

    if (cfg.arcHeight < 8 || cfg.arcHeight > 40) {
        return fail("arc_height must be 8..40");
    }
    if (cfg.arcOffset < 2 || cfg.arcOffset >= cfg.arcHeight) {
        return fail("arc_offset must be 2..arc_height-1");
    }
    if (cfg.arcBulge < 0 || cfg.arcBulge > 8) {
        return fail("arc_bulge must be 0..8");
    }
    if (cfg.arcStroke < 1 || cfg.arcStroke > 3) {
        return fail("arc_stroke must be 1..3");
    }
    if (cfg.stagger < 0 || cfg.stagger > 8) {
        return fail("stagger must be 0..8");
    }
    if (cfg.swayMax < 0 || cfg.swayMax > 8) {
        return fail("sway_max must be 0..8");
    }
    if (cfg.swayPhaseStep < 0 || cfg.swayPhaseStep > 32) {
        return fail("sway_phase_step must be 0..32");
    }
    if (cfg.swayStepMs < 10 || cfg.swayStepMs > 2000) {
        return fail("sway_step_ms must be 10..2000");
    }
    if (cfg.weedCount > AQUARIUM_MAX_WEEDS) {
        return fail("too many weeds");
    }
    if (cfg.weedGrowS > 3600) {
        return fail("weed_grow_s must be 0..3600");
    }
    if (cfg.fishEatChance > 100) {
        return fail("fish_eat_chance must be 0..100");
    }
    if (cfg.bubbleChance > 100) {
        return fail("bubble_chance must be 0..100");
    }
    if (cfg.bubbleRise < 2 || cfg.bubbleRise > 80) {
        return fail("bubble_rise must be 2..80");
    }
    if (cfg.bubbleEveryMs < 300 || cfg.bubbleEveryMs > 10000) {
        return fail("bubble_every_ms must be 300..10000");
    }
    for (uint8_t i = 0; i < cfg.weedCount; ++i) {
        if (cfg.weeds[i].brackets < 1 || cfg.weeds[i].brackets > 4) {
            return fail("brackets must be 1..4");
        }
        if (cfg.weeds[i].axisX < 0 || cfg.weeds[i].axisX > DisplayManager::getGfx()->width() - 1) {
            return fail("weed x out of screen");
        }
    }
    if (cfg.fishCount > AQUARIUM_MAX_FISH) {
        return fail("fish_count must be 0..6");
    }
    if (cfg.fishScaleMin < 1 || cfg.fishScaleMax > FISH_MAX_SCALE || cfg.fishScaleMin > cfg.fishScaleMax) {
        return fail("fish scale must be 1..3 and min <= max");
    }
    if (cfg.fishSpeedMin < 5 || cfg.fishSpeedMax > 300 || cfg.fishSpeedMin > cfg.fishSpeedMax) {
        return fail("fish speed must be 5..300 and min <= max");
    }
    if (cfg.fishBobMin < 0 || cfg.fishBobMax > FISH_MAX_BOB || cfg.fishBobMin > cfg.fishBobMax) {
        return fail("fish bob must be 0..6 and min <= max");
    }

    int16_t width = 0;
    int16_t height = 0;
    if (!computeCanvas(cfg, width, height)) {
        return fail("scene too large, lower the sizes");
    }

    const int16_t fishCanvasW =
        static_cast<int16_t>((((FISH_MAX_CELLS - 1) * CELL_W) + GLYPH_W) * cfg.fishScaleMax + FISH_MAX_STEP);
    const int16_t fishCanvasH = static_cast<int16_t>((GLYPH_H * cfg.fishScaleMax) + FISH_MAX_BOB +
                                                     (2 * FISH_WAVE_AMPLITUDE * cfg.fishScaleMax));
    const size_t fishPixels = static_cast<size_t>(fishCanvasW) * static_cast<size_t>(fishCanvasH);
    if (fishPixels > MAX_FISH_CANVAS_PIXELS) {
        return fail("fish too large, lower fish_scale_max");
    }

    if (fishPixels > s_fishCanvasPixels) {
        auto* buffer = new (std::nothrow) uint16_t[fishPixels];
        if (buffer == nullptr) {
            return fail("out of memory for the fish buffer");
        }

        delete[] s_fishCanvas;
        s_fishCanvas = buffer;
        s_fishCanvasPixels = fishPixels;
    }

    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixels > s_canvasPixels) {
        auto* buffer = new (std::nothrow) uint16_t[pixels];
        if (buffer == nullptr) {
            return fail("out of memory for the scene buffer");
        }

        delete[] s_canvas;
        s_canvas = buffer;
        s_canvasPixels = pixels;
    }

    bool weedsChanged = (cfg.weedCount != s_cfg.weedCount);
    for (uint8_t i = 0; !weedsChanged && i < cfg.weedCount; ++i) {
        weedsChanged =
            (cfg.weeds[i].axisX != s_cfg.weeds[i].axisX) || (cfg.weeds[i].brackets != s_cfg.weeds[i].brackets);
    }

    s_cfg = cfg;
    s_cfgReady = true;

    // Moving or reshaping a plant starts it over as an intact one, and so does switching bites off.
    if (weedsChanged || !cfg.weedEat) {
        for (uint8_t i = 0; i < AQUARIUM_MAX_WEEDS; ++i) {
            s_eaten[i] = 0;
            s_regrowAt[i] = 0;
        }

        // Fish that were eating have to let go too, otherwise they keep hovering over a plant that
        // changed underneath them and hold it reserved for nobody.
        for (uint8_t i = 0; i < cfg.fishCount; ++i) {
            s_fish[i].eatWeed = -1;
            s_fish[i].eatStartMs = 0;
            s_fish[i].eatEndMs = 0;
        }
    }

    s_canvasW = width;
    s_canvasH = height;
    s_fishCanvasW = fishCanvasW;
    s_fishCanvasH = fishCanvasH;
    rebuildArcShape(s_cfg);
    // Switching between lanes and free swimming re-deals the fish, so each mode starts from its own
    // placement instead of leaving them half-way between the two.
    if (cfg.fishSwim != previousSwim) {
        s_fishReady = false;
    }

    layoutFish(previousFishCount);
    s_needsRedraw = true;

    Logger::info(("Scene applied: canvas " + String(s_canvasW) + "x" + String(s_canvasH) + ", fish buffer " +
                  String(s_fishCanvasW) + "x" + String(s_fishCanvasH))
                     .c_str(),
                 "Aquarium");

    return true;
}

void AquariumManager::loadConfig() {
    AquariumConfig cfg = defaults();

    File file = LittleFS.open(CONFIG_PATH, "r");
    if (file) {
        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, file);
        file.close();

        if (!error) {
            cfg.arcHeight = doc["arc_height"] | cfg.arcHeight;
            cfg.arcOffset = doc["arc_offset"] | cfg.arcOffset;
            cfg.arcBulge = doc["arc_bulge"] | cfg.arcBulge;
            cfg.arcStroke = doc["arc_stroke"] | cfg.arcStroke;
            cfg.stagger = doc["stagger"] | cfg.stagger;
            cfg.swayMax = doc["sway_max"] | cfg.swayMax;
            cfg.swayPhaseStep = doc["sway_phase_step"] | cfg.swayPhaseStep;
            cfg.swayStepMs = doc["sway_step_ms"] | cfg.swayStepMs;
            cfg.color = doc["color"] | cfg.color;
            cfg.fishCount = doc["fish_count"] | cfg.fishCount;
            cfg.fishScaleMin = doc["fish_scale_min"] | cfg.fishScaleMin;
            cfg.fishScaleMax = doc["fish_scale_max"] | cfg.fishScaleMax;
            cfg.fishSpeedMin = doc["fish_speed_min"] | cfg.fishSpeedMin;
            cfg.fishSpeedMax = doc["fish_speed_max"] | cfg.fishSpeedMax;
            cfg.fishBobMin = doc["fish_bob_min"] | cfg.fishBobMin;
            cfg.fishBobMax = doc["fish_bob_max"] | cfg.fishBobMax;
            cfg.fishTurnAnim = doc["fish_turn_anim"] | cfg.fishTurnAnim;
            cfg.fishSwim = (String(doc["fish_swim"] | "free") == "lanes") ? FishSwim::Lanes : FishSwim::Free;
            cfg.bubbles = doc["bubbles"] | cfg.bubbles;
            cfg.bubbleChance = doc["bubble_chance"] | cfg.bubbleChance;
            cfg.bubbleRise = doc["bubble_rise"] | cfg.bubbleRise;
            cfg.bubbleEveryMs = static_cast<uint16_t>(doc["bubble_every_ms"] | static_cast<int>(cfg.bubbleEveryMs));
            cfg.weedGrowS = doc["weed_grow_s"] | cfg.weedGrowS;
            cfg.weedEat = doc["weed_eat"] | cfg.weedEat;
            cfg.fishEatChance = doc["fish_eat_chance"] | cfg.fishEatChance;

            JsonArray weeds = doc["weeds"].as<JsonArray>();
            if (!weeds.isNull() && weeds.size() > 0) {
                uint8_t count = 0;
                for (JsonObject weed : weeds) {
                    if (count >= AQUARIUM_MAX_WEEDS) {
                        break;
                    }
                    cfg.weeds[count].axisX = weed["x"] | cfg.weeds[count].axisX;
                    cfg.weeds[count].brackets = weed["brackets"] | cfg.weeds[count].brackets;
                    ++count;
                }
                cfg.weedCount = count;
            }
        } else {
            Logger::warn("Invalid aquarium config, using defaults", "Aquarium");
        }
    }

    if (!applyConfig(cfg)) {
        Logger::warn((String("Aquarium config rejected: ") + s_lastError).c_str(), "Aquarium");
        applyConfig(defaults());
    }
}

bool AquariumManager::saveConfig() {
    File file = LittleFS.open(CONFIG_PATH, "w");
    if (!file) {
        Logger::error("Failed to open aquarium config for writing", "Aquarium");

        return false;
    }

    JsonDocument doc;
    doc["arc_height"] = s_cfg.arcHeight;
    doc["arc_offset"] = s_cfg.arcOffset;
    doc["arc_bulge"] = s_cfg.arcBulge;
    doc["arc_stroke"] = s_cfg.arcStroke;
    doc["stagger"] = s_cfg.stagger;
    doc["sway_max"] = s_cfg.swayMax;
    doc["sway_phase_step"] = s_cfg.swayPhaseStep;
    doc["sway_step_ms"] = s_cfg.swayStepMs;
    doc["color"] = s_cfg.color;
    doc["fish_count"] = s_cfg.fishCount;
    doc["fish_scale_min"] = s_cfg.fishScaleMin;
    doc["fish_scale_max"] = s_cfg.fishScaleMax;
    doc["fish_speed_min"] = s_cfg.fishSpeedMin;
    doc["fish_speed_max"] = s_cfg.fishSpeedMax;
    doc["fish_bob_min"] = s_cfg.fishBobMin;
    doc["fish_bob_max"] = s_cfg.fishBobMax;
    doc["fish_turn_anim"] = s_cfg.fishTurnAnim;
    doc["fish_swim"] = (s_cfg.fishSwim == FishSwim::Lanes) ? "lanes" : "free";
    doc["bubbles"] = s_cfg.bubbles;
    doc["bubble_chance"] = s_cfg.bubbleChance;
    doc["bubble_rise"] = s_cfg.bubbleRise;
    doc["bubble_every_ms"] = s_cfg.bubbleEveryMs;
    doc["weed_grow_s"] = s_cfg.weedGrowS;
    doc["weed_eat"] = s_cfg.weedEat;
    doc["fish_eat_chance"] = s_cfg.fishEatChance;

    JsonArray weeds = doc["weeds"].to<JsonArray>();
    for (uint8_t i = 0; i < s_cfg.weedCount; ++i) {
        JsonObject weed = weeds.add<JsonObject>();
        weed["x"] = s_cfg.weeds[i].axisX;
        weed["brackets"] = s_cfg.weeds[i].brackets;
    }

    const size_t written = serializeJson(doc, file);
    file.close();

    if (written == 0) {
        Logger::error("Failed to write aquarium config", "Aquarium");

        return false;
    }

    Logger::info("Aquarium config saved", "Aquarium");

    return true;
}

void AquariumManager::begin() {
    if (s_active) {
        return;
    }

    randomSeed(micros() ^ ESP.getChipId());
    loadConfig();

    s_active = true;
    s_paused = false;
    s_needsRedraw = true;
    s_lastFrameMs = millis() - FRAME_INTERVAL_MS;

    update();
}

void AquariumManager::update() {
    if (!s_active || s_paused || (s_canvas == nullptr)) {
        return;
    }

    // A GIF or an OTA screen owns the display: redraw everything once it is done.
    if (DisplayManager::isGifPlaying()) {
        s_needsRedraw = true;

        return;
    }

    const uint32_t now = millis();
    if (!s_needsRedraw && (now - s_lastFrameMs) < FRAME_INTERVAL_MS) {
        return;
    }

    if (s_needsRedraw) {
        DisplayManager::clearScreen();
        s_needsRedraw = false;
    }

    s_lastFrameMs = now;
    renderFrame();
}

void AquariumManager::pause(bool paused) {
    s_paused = paused;

    if (!paused) {
        s_needsRedraw = true;
    }
}

bool AquariumManager::isActive() { return s_active && !s_paused; }
