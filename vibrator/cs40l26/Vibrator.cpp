/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Vibrator.h"

#include <android-base/properties.h>
#include <hardware/hardware.h>
#include <hardware/vibrator.h>
#include <log/log.h>
#include <utils/Trace.h>

#include <cinttypes>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>

#include "Stats.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof((x)) / sizeof((x)[0]))
#endif

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

#ifdef HAPTIC_TRACE
#define HAPTICS_TRACE(...) ALOGD(__VA_ARGS__)
#else
#define HAPTICS_TRACE(...)
#endif

static constexpr uint8_t FF_CUSTOM_DATA_LEN = 2;
static constexpr uint16_t FF_CUSTOM_DATA_LEN_MAX_COMP = 2044;  // (COMPOSE_SIZE_MAX + 1) * 8 + 4
static constexpr uint16_t FF_CUSTOM_DATA_LEN_MAX_PWLE = 2302;

static constexpr uint32_t WAVEFORM_DOUBLE_CLICK_SILENCE_MS = 100;

static constexpr uint32_t WAVEFORM_LONG_VIBRATION_THRESHOLD_MS = 50;

static constexpr uint8_t VOLTAGE_SCALE_MAX = 100;

static constexpr int8_t MAX_COLD_START_LATENCY_MS = 6;  // I2C Transaction + DSP Return-From-Standby
static constexpr uint32_t MIN_ON_OFF_INTERVAL_US = 8500;  // SVC initialization time
static constexpr int8_t MAX_PAUSE_TIMING_ERROR_MS = 1;  // ALERT Irq Handling
static constexpr uint32_t MAX_TIME_MS = UINT16_MAX;

static constexpr auto ASYNC_COMPLETION_TIMEOUT = std::chrono::milliseconds(100);
static constexpr auto POLLING_TIMEOUT = 20;
static constexpr int32_t COMPOSE_DELAY_MAX_MS = 10000;

/* nsections is 8 bits. Need to preserve 1 section for the first delay before the first effect. */
static constexpr int32_t COMPOSE_SIZE_MAX = 254;
static constexpr int32_t COMPOSE_PWLE_SIZE_MAX_DEFAULT = 127;

// Measured resonant frequency, f0_measured, is represented by Q10.14 fixed
// point format on cs40l26 devices. The expression to calculate f0 is:
//   f0 = f0_measured / 2^Q14_BIT_SHIFT
// See the LRA Calibration Support documentation for more details.
static constexpr int32_t Q14_BIT_SHIFT = 14;

// Measured ReDC. The LRA series resistance (ReDC), expressed as follows
// redc(ohms) = redc_measured / 2^Q15_BIT_SHIFT.
// This value represents the unit-specific ReDC input to the click compensation
// algorithm. It can be overwritten at a later time by writing to the redc_stored
// sysfs control.
// See the LRA Calibration Support documentation for more details.
static constexpr int32_t Q15_BIT_SHIFT = 15;

// Measured Q factor, q_measured, is represented by Q8.16 fixed
// point format on cs40l26 devices. The expression to calculate q is:
//   q = q_measured / 2^Q16_BIT_SHIFT
// See the LRA Calibration Support documentation for more details.
static constexpr int32_t Q16_BIT_SHIFT = 16;

static constexpr int32_t COMPOSE_PWLE_PRIMITIVE_DURATION_MAX_MS = 16383;

static constexpr uint32_t WT_LEN_CALCD = 0x00800000;
static constexpr uint8_t PWLE_CHIRP_BIT = 0x8;  // Dynamic/static frequency and voltage
static constexpr uint8_t PWLE_BRAKE_BIT = 0x4;
static constexpr uint8_t PWLE_AMP_REG_BIT = 0x2;

static constexpr float PWLE_LEVEL_MIN = 0.0;
static constexpr float PWLE_LEVEL_MAX = 1.0;
static constexpr float CS40L26_PWLE_LEVEL_MIN = -1.0;
static constexpr float CS40L26_PWLE_LEVEL_MAX = 0.9995118;
static constexpr float PWLE_FREQUENCY_RESOLUTION_HZ = 1.00;
static constexpr float PWLE_FREQUENCY_MIN_HZ = 30.0f;
static constexpr float RESONANT_FREQUENCY_DEFAULT = 145.0f;
static constexpr float PWLE_FREQUENCY_MAX_HZ = 300.0f;
static constexpr float PWLE_BW_MAP_SIZE =
        1 + ((PWLE_FREQUENCY_MAX_HZ - PWLE_FREQUENCY_MIN_HZ) / PWLE_FREQUENCY_RESOLUTION_HZ);

static uint16_t amplitudeToScale(float amplitude, float maximum) {
    float ratio = 100; /* Unit: % */
    if (maximum != 0)
        ratio = amplitude / maximum * 100;

    if (maximum == 0 || ratio > 100)
        ratio = 100;

    return std::round(ratio);
}

enum WaveformBankID : uint8_t {
    RAM_WVFRM_BANK,
    ROM_WVFRM_BANK,
    OWT_WVFRM_BANK,
};

enum WaveformIndex : uint16_t {
    /* Physical waveform */
    WAVEFORM_LONG_VIBRATION_EFFECT_INDEX = 0,
    WAVEFORM_RESERVED_INDEX_1 = 1,
    WAVEFORM_CLICK_INDEX = 2,
    WAVEFORM_SHORT_VIBRATION_EFFECT_INDEX = 3,
    WAVEFORM_THUD_INDEX = 4,
    WAVEFORM_SPIN_INDEX = 5,
    WAVEFORM_QUICK_RISE_INDEX = 6,
    WAVEFORM_SLOW_RISE_INDEX = 7,
    WAVEFORM_QUICK_FALL_INDEX = 8,
    WAVEFORM_LIGHT_TICK_INDEX = 9,
    WAVEFORM_LOW_TICK_INDEX = 10,
    WAVEFORM_RESERVED_MFG_1,
    WAVEFORM_RESERVED_MFG_2,
    WAVEFORM_RESERVED_MFG_3,
    WAVEFORM_MAX_PHYSICAL_INDEX,
    /* OWT waveform */
    WAVEFORM_COMPOSE = WAVEFORM_MAX_PHYSICAL_INDEX,
    WAVEFORM_PWLE,
    /*
     * Refer to <linux/input.h>, the WAVEFORM_MAX_INDEX must not exceed 96.
     * #define FF_GAIN      0x60  // 96 in decimal
     * #define FF_MAX_EFFECTS   FF_GAIN
     */
    WAVEFORM_MAX_INDEX,
};

std::vector<CompositePrimitive> defaultSupportedPrimitives = {
        ndk::enum_range<CompositePrimitive>().begin(), ndk::enum_range<CompositePrimitive>().end()};

enum vibe_state {
    VIBE_STATE_STOPPED = 0,
    VIBE_STATE_HAPTIC,
    VIBE_STATE_ASP,
};

std::mutex mActiveId_mutex;  // protects mActiveId

static int min(int x, int y) {
    return x < y ? x : y;
}

static int floatToUint16(float input, uint16_t *output, float scale, float min, float max) {
    if (input < min || input > max)
        return -ERANGE;

    *output = roundf(input * scale);
    return 0;
}

struct dspmem_chunk {
    std::unique_ptr<uint8_t[]> head;
    uint8_t *current;
    uint8_t *max;
    int bytes = 0;

    uint32_t cache = 0;
    int cachebits = 0;

    dspmem_chunk(int size) : head(new uint8_t[size]{0x00}) {
        current = head.get();
        max = current + size;
    }
};

static bool dspmem_chunk_end(const struct dspmem_chunk &ch) {
    return ch.current == ch.max;
}

static int dspmem_chunk_bytes(const struct dspmem_chunk &ch) {
    return ch.bytes;
}

static int dspmem_chunk_write(struct dspmem_chunk *ch, int nbits, uint32_t val) {
    HAPTICS_TRACE("     dspmem_chunk_write(ch, nbits:%d, val:%u)", nbits, val);
    int nwrite, i;

    nwrite = min(24 - ch->cachebits, nbits);
    ch->cache <<= nwrite;
    ch->cache |= val >> (nbits - nwrite);
    ch->cachebits += nwrite;
    nbits -= nwrite;

    if (ch->cachebits == 24) {
        if (dspmem_chunk_end(*ch))
            return -ENOSPC;

        ch->cache &= 0xFFFFFF;
        for (i = 0; i < sizeof(ch->cache); i++, ch->cache <<= 8)
            *ch->current++ = (ch->cache & 0xFF000000) >> 24;

        ch->bytes += sizeof(ch->cache);
        ch->cachebits = 0;
    }

    if (nbits)
        return dspmem_chunk_write(ch, nbits, val);

    return 0;
}

static int dspmem_chunk_flush(struct dspmem_chunk *ch) {
    HAPTICS_TRACE("     dspmem_chunk_flush(ch)");
    if (!ch->cachebits)
        return 0;

    return dspmem_chunk_write(ch, 24 - ch->cachebits, 0);
}

// Discrete points of frequency:max_level pairs around resonant(145Hz default) frequency
// Initialize the actuator LUXSHARE_ICT_081545 limits to 0.447 and others 1.0
#if defined(LUXSHARE_ICT_081545)
static std::map<float, float> discretePwleMaxLevels = {
        {120.0, 0.447}, {130.0, 0.346}, {140.0, 0.156}, {145.0, 0.1},
        {150.0, 0.167}, {160.0, 0.391}, {170.0, 0.447}};
std::vector<float> pwleMaxLevelLimitMap(PWLE_BW_MAP_SIZE, 0.447);
#else
static std::map<float, float> discretePwleMaxLevels = {};
std::vector<float> pwleMaxLevelLimitMap(PWLE_BW_MAP_SIZE, 1.0);
#endif

static float redcToFloat(std::string *caldata) {
    return static_cast<float>(std::stoul(*caldata, nullptr, 16)) / (1 << Q15_BIT_SHIFT);
}

Vibrator::Vibrator(std::unique_ptr<HwApi> hwapi, std::unique_ptr<HwCal> hwcal,
                   std::unique_ptr<StatsApi> statsapi)
    : mHwApi(std::move(hwapi)),
      mHwCal(std::move(hwcal)),
      mStatsApi(std::move(statsapi)),
      mAsyncHandle(std::async([] {})) {
    int32_t longFrequencyShift;
    std::string caldata{8, '0'};
    uint32_t calVer;
    const std::string INPUT_EVENT_NAME = std::getenv("INPUT_EVENT_NAME") ?: "";

    mFfEffects.resize(WAVEFORM_MAX_INDEX);
    mEffectDurations.resize(WAVEFORM_MAX_INDEX);
    mEffectDurations = {
            1000, 100, 12, 1000, 300, 130, 150, 500, 100, 5, 12, 1000, 1000, 1000,
    }; /* 11+3 waveforms. The duration must < UINT16_MAX */

    uint8_t effectIndex;
    for (effectIndex = 0; effectIndex < WAVEFORM_MAX_INDEX; effectIndex++) {
        if (effectIndex < WAVEFORM_MAX_PHYSICAL_INDEX) {
            /* Initialize physical waveforms. */
            mFfEffects[effectIndex] = {
                    .type = FF_PERIODIC,
                    .id = -1,
                    .replay.length = static_cast<uint16_t>(mEffectDurations[effectIndex]),
                    .u.periodic.waveform = FF_CUSTOM,
                    .u.periodic.custom_data = new int16_t[2]{RAM_WVFRM_BANK, effectIndex},
                    .u.periodic.custom_len = FF_CUSTOM_DATA_LEN,
            };
            // Bypass the waveform update due to different input name
            if (INPUT_EVENT_NAME.find("cs40l26") != std::string::npos) {
                if (!mHwApi->setFFEffect(
                            &mFfEffects[effectIndex],
                            static_cast<uint16_t>(mFfEffects[effectIndex].replay.length))) {
                    mStatsApi->logError(kHwApiError);
                    ALOGE("Failed upload effect %d (%d): %s", effectIndex, errno, strerror(errno));
                }
            }
            if (mFfEffects[effectIndex].id != effectIndex) {
                ALOGW("Unexpected effect index: %d -> %d", effectIndex, mFfEffects[effectIndex].id);
            }
        } else {
            /* Initiate placeholders for OWT effects. */
            mFfEffects[effectIndex] = {
                    .type = FF_PERIODIC,
                    .id = -1,
                    .replay.length = 0,
                    .u.periodic.waveform = FF_CUSTOM,
                    .u.periodic.custom_data = nullptr,
                    .u.periodic.custom_len = 0,
            };
        }
    }

    if (mHwCal->getF0(&caldata)) {
        mHwApi->setF0(caldata);
        mResonantFrequency =
                static_cast<float>(std::stoul(caldata, nullptr, 16)) / (1 << Q14_BIT_SHIFT);
    } else {
        mStatsApi->logError(kHwCalError);
        ALOGE("Failed to get resonant frequency (%d): %s, using default resonant HZ: %f", errno,
              strerror(errno), RESONANT_FREQUENCY_DEFAULT);
        mResonantFrequency = RESONANT_FREQUENCY_DEFAULT;
    }
    if (mHwCal->getRedc(&caldata)) {
        mHwApi->setRedc(caldata);
        mRedc = redcToFloat(&caldata);
    }
    if (mHwCal->getQ(&caldata)) {
        mHwApi->setQ(caldata);
    }

    mHwCal->getLongFrequencyShift(&longFrequencyShift);
    if (longFrequencyShift > 0) {
        mF0Offset = longFrequencyShift * std::pow(2, 14);
    } else if (longFrequencyShift < 0) {
        mF0Offset = std::pow(2, 24) - std::abs(longFrequencyShift) * std::pow(2, 14);
    } else {
        mF0Offset = 0;
    }

    mHwCal->getVersion(&calVer);
    if (calVer == 2) {
        mHwCal->getTickVolLevels(&mTickEffectVol);
        mHwCal->getClickVolLevels(&mClickEffectVol);
        mHwCal->getLongVolLevels(&mLongEffectVol);
    } else {
        ALOGD("Unsupported calibration version: %u!", calVer);
    }
    HAPTICS_TRACE("Vibrator(hwapi, hwcal:%u)", calVer);

    mHwApi->setF0CompEnable(mHwCal->isF0CompEnabled());
    mHwApi->setRedcCompEnable(mHwCal->isRedcCompEnabled());

    mIsUnderExternalControl = false;

    mIsChirpEnabled = mHwCal->isChirpEnabled();

    mHwCal->getSupportedPrimitives(&mSupportedPrimitivesBits);
    if (mSupportedPrimitivesBits > 0) {
        for (auto e : defaultSupportedPrimitives) {
            if (mSupportedPrimitivesBits & (1 << uint32_t(e))) {
                mSupportedPrimitives.emplace_back(e);
            }
        }
    } else {
        for (auto e : defaultSupportedPrimitives) {
            mSupportedPrimitivesBits |= (1 << uint32_t(e));
        }
        mSupportedPrimitives = defaultSupportedPrimitives;
    }

    mHwApi->setMinOnOffInterval(MIN_ON_OFF_INTERVAL_US);

    createPwleMaxLevelLimitMap();
    createBandwidthAmplitudeMap();
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t *_aidl_return) {
    HAPTICS_TRACE("getCapabilities(_aidl_return)");
    ATRACE_NAME("Vibrator::getCapabilities");

    int32_t ret = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                  IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_GET_RESONANT_FREQUENCY |
                  IVibrator::CAP_GET_Q_FACTOR;
    if (hasHapticAlsaDevice()) {
        ret |= IVibrator::CAP_EXTERNAL_CONTROL;
    } else {
        mStatsApi->logError(kAlsaFailError);
        ALOGE("No haptics ALSA device");
    }
    if (mHwApi->hasOwtFreeSpace()) {
        ret |= IVibrator::CAP_COMPOSE_EFFECTS;
        if (mIsChirpEnabled) {
            ret |= IVibrator::CAP_FREQUENCY_CONTROL | IVibrator::CAP_COMPOSE_PWLE_EFFECTS;
        }
    }
    *_aidl_return = ret;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    HAPTICS_TRACE("off()");
    ATRACE_NAME("Vibrator::off");
    bool ret{true};
    const std::scoped_lock<std::mutex> lock(mActiveId_mutex);

    if (mActiveId >= 0) {
        /* Stop the active effect. */
        if (!mHwApi->setFFPlay(mActiveId, false)) {
            mStatsApi->logError(kHwApiError);
            ALOGE("Failed to stop effect %d (%d): %s", mActiveId, errno, strerror(errno));
            ret = false;
        }

        if ((mActiveId >= WAVEFORM_MAX_PHYSICAL_INDEX) &&
            (!mHwApi->eraseOwtEffect(mActiveId, &mFfEffects))) {
            mStatsApi->logError(kHwApiError);
            ALOGE("Failed to clean up the composed effect %d", mActiveId);
            ret = false;
        }
    } else {
        ALOGV("Vibrator is already off");
    }

    mActiveId = -1;
    setGlobalAmplitude(false);
    if (mF0Offset) {
        mHwApi->setF0Offset(0);
    }

    if (ret) {
        return ndk::ScopedAStatus::ok();
    } else {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback> &callback) {
    ATRACE_NAME(StringPrintf("Vibrator::on %dms", timeoutMs).c_str());
    HAPTICS_TRACE("on(timeoutMs:%d, callback)", timeoutMs);

    mStatsApi->logLatencyStart(kWaveformEffectLatency);
    if (timeoutMs > MAX_TIME_MS) {
        mStatsApi->logError(kBadTimeoutError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const uint16_t index = (timeoutMs < WAVEFORM_LONG_VIBRATION_THRESHOLD_MS)
                                   ? WAVEFORM_SHORT_VIBRATION_EFFECT_INDEX
                                   : WAVEFORM_LONG_VIBRATION_EFFECT_INDEX;
    if (MAX_COLD_START_LATENCY_MS <= MAX_TIME_MS - timeoutMs) {
        timeoutMs += MAX_COLD_START_LATENCY_MS;
    }
    setGlobalAmplitude(true);
    if (mF0Offset) {
        mHwApi->setF0Offset(mF0Offset);
    }

    mStatsApi->logWaveform(index, timeoutMs);
    return on(timeoutMs, index, nullptr /*ignored*/, callback);
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                     const std::shared_ptr<IVibratorCallback> &callback,
                                     int32_t *_aidl_return) {
    ATRACE_NAME(StringPrintf("Vibrator::perform %s,%s", toString(effect).c_str(),
                             toString(strength).c_str())
                        .c_str());
    HAPTICS_TRACE("perform(effect:%s, strength:%s, callback, _aidl_return)",
                  toString(effect).c_str(), toString(strength).c_str());

    mStatsApi->logLatencyStart(kPrebakedEffectLatency);

    return performEffect(effect, strength, callback, _aidl_return);
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect> *_aidl_return) {
    HAPTICS_TRACE("getSupportedEffects(_aidl_return)");
    *_aidl_return = {Effect::TEXTURE_TICK, Effect::TICK, Effect::CLICK, Effect::HEAVY_CLICK,
                     Effect::DOUBLE_CLICK};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    HAPTICS_TRACE("setAmplitude(amplitude:%f)", amplitude);
    ATRACE_NAME("Vibrator::setAmplitude");
    if (amplitude <= 0.0f || amplitude > 1.0f) {
        mStatsApi->logError(kBadAmplitudeError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    mLongEffectScale = amplitude;
    if (!isUnderExternalControl()) {
        return setGlobalAmplitude(true);
    } else {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool enabled) {
    HAPTICS_TRACE("setExternalControl(enabled:%u)", enabled);
    ATRACE_NAME("Vibrator::setExternalControl");
    setGlobalAmplitude(enabled);

    if (mHasHapticAlsaDevice || mConfigHapticAlsaDeviceDone || hasHapticAlsaDevice()) {
        if (!mHwApi->setHapticPcmAmp(&mHapticPcm, enabled, mCard, mDevice)) {
            mStatsApi->logError(kHwApiError);
            ALOGE("Failed to %s haptic pcm device: %d", (enabled ? "enable" : "disable"), mDevice);
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
    } else {
        mStatsApi->logError(kAlsaFailError);
        ALOGE("No haptics ALSA device");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    mIsUnderExternalControl = enabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t *maxDelayMs) {
    HAPTICS_TRACE("getCompositionDelayMax(maxDelayMs)");
    ATRACE_NAME("Vibrator::getCompositionDelayMax");
    *maxDelayMs = COMPOSE_DELAY_MAX_MS;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t *maxSize) {
    HAPTICS_TRACE("getCompositionSizeMax(maxSize)");
    ATRACE_NAME("Vibrator::getCompositionSizeMax");
    *maxSize = COMPOSE_SIZE_MAX;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive> *supported) {
    HAPTICS_TRACE("getSupportedPrimitives(supported)");
    *supported = mSupportedPrimitives;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t *durationMs) {
    HAPTICS_TRACE("getPrimitiveDuration(primitive:%s, durationMs)", toString(primitive).c_str());
    ndk::ScopedAStatus status;
    uint32_t effectIndex;
    if (primitive != CompositePrimitive::NOOP) {
        status = getPrimitiveDetails(primitive, &effectIndex);
        if (!status.isOk()) {
            return status;
        }

        *durationMs = mEffectDurations[effectIndex];
    } else {
        *durationMs = 0;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect> &composite,
                                     const std::shared_ptr<IVibratorCallback> &callback) {
    ATRACE_NAME(StringPrintf("Vibrator::compose size=%zu", composite.size()).c_str());
    HAPTICS_TRACE("compose(composite, callback)");
    uint16_t size;
    uint16_t nextEffectDelay;
    uint16_t totalDuration = 0;

    mStatsApi->logLatencyStart(kCompositionEffectLatency);

    if (composite.size() > COMPOSE_SIZE_MAX || composite.empty()) {
        mStatsApi->logError(kBadCompositeError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    /* Check if there is a wait before the first effect. */
    nextEffectDelay = composite.front().delayMs;
    totalDuration += nextEffectDelay;
    if (nextEffectDelay > COMPOSE_DELAY_MAX_MS || nextEffectDelay < 0) {
        mStatsApi->logError(kBadCompositeError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    } else if (nextEffectDelay > 0) {
        size = composite.size() + 1;
    } else {
        size = composite.size();
    }

    dspmem_chunk ch(FF_CUSTOM_DATA_LEN_MAX_COMP);
    dspmem_chunk_write(&ch, 8, 0);                      /* Padding */
    dspmem_chunk_write(&ch, 8, (uint8_t)(0xFF & size)); /* nsections */
    dspmem_chunk_write(&ch, 8, 0);                      /* repeat */
    uint8_t header_count = dspmem_chunk_bytes(ch);

    /* Insert 1 section for a wait before the first effect. */
    if (nextEffectDelay) {
        dspmem_chunk_write(&ch, 32, 0); /* amplitude, index, repeat & flags */
        dspmem_chunk_write(&ch, 16, (uint16_t)(0xFFFF & nextEffectDelay)); /* delay */
    }

    for (uint32_t i_curr = 0, i_next = 1; i_curr < composite.size(); i_curr++, i_next++) {
        auto &e_curr = composite[i_curr];
        uint32_t effectIndex = 0;
        uint32_t effectVolLevel = 0;
        if (e_curr.scale < 0.0f || e_curr.scale > 1.0f) {
            mStatsApi->logError(kBadCompositeError);
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }

        if (e_curr.primitive != CompositePrimitive::NOOP) {
            ndk::ScopedAStatus status;
            status = getPrimitiveDetails(e_curr.primitive, &effectIndex);
            if (!status.isOk()) {
                return status;
            }
            effectVolLevel = intensityToVolLevel(e_curr.scale, effectIndex);
            totalDuration += mEffectDurations[effectIndex];
        }

        /* Fetch the next composite effect delay and fill into the current section */
        nextEffectDelay = 0;
        if (i_next < composite.size()) {
            auto &e_next = composite[i_next];
            int32_t delay = e_next.delayMs;

            if (delay > COMPOSE_DELAY_MAX_MS || delay < 0) {
                mStatsApi->logError(kBadCompositeError);
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
            }
            nextEffectDelay = delay;
            totalDuration += delay;
        }

        if (effectIndex == 0 && nextEffectDelay == 0) {
            mStatsApi->logError(kBadCompositeError);
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        mStatsApi->logPrimitive(effectIndex);

        dspmem_chunk_write(&ch, 8, (uint8_t)(0xFF & effectVolLevel));      /* amplitude */
        dspmem_chunk_write(&ch, 8, (uint8_t)(0xFF & effectIndex));         /* index */
        dspmem_chunk_write(&ch, 8, 0);                                     /* repeat */
        dspmem_chunk_write(&ch, 8, 0);                                     /* flags */
        dspmem_chunk_write(&ch, 16, (uint16_t)(0xFFFF & nextEffectDelay)); /* delay */
    }
    dspmem_chunk_flush(&ch);
    if (header_count == dspmem_chunk_bytes(ch)) {
        mStatsApi->logError(kComposeFailError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    } else {
        mFfEffects[WAVEFORM_COMPOSE].replay.length = totalDuration;
        return performEffect(WAVEFORM_MAX_INDEX /*ignored*/, VOLTAGE_SCALE_MAX /*ignored*/, &ch,
                             callback);
    }
}

ndk::ScopedAStatus Vibrator::on(uint32_t timeoutMs, uint32_t effectIndex, dspmem_chunk *ch,
                                const std::shared_ptr<IVibratorCallback> &callback) {
    HAPTICS_TRACE("on(timeoutMs:%u, effectIndex:%u, ch, callback)", timeoutMs, effectIndex);
    ndk::ScopedAStatus status = ndk::ScopedAStatus::ok();

    if (effectIndex >= FF_MAX_EFFECTS) {
        mStatsApi->logError(kBadEffectError);
        ALOGE("Invalid waveform index %d", effectIndex);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (mAsyncHandle.wait_for(ASYNC_COMPLETION_TIMEOUT) != std::future_status::ready) {
        mStatsApi->logError(kAsyncFailError);
        ALOGE("Previous vibration pending: prev: %d, curr: %d", mActiveId, effectIndex);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    if (ch) {
        /* Upload OWT effect. */
        if (ch->head == nullptr) {
            mStatsApi->logError(kBadCompositeError);
            ALOGE("Invalid OWT bank");
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        bool isPwle = (*reinterpret_cast<uint16_t *>(ch->head.get()) != 0x0000);
        effectIndex = isPwle ? WAVEFORM_PWLE : WAVEFORM_COMPOSE;

        uint32_t freeBytes;
        mHwApi->getOwtFreeSpace(&freeBytes);
        if (dspmem_chunk_bytes(*ch) > freeBytes) {
            mStatsApi->logError(kBadCompositeError);
            ALOGE("Invalid OWT length: Effect %d: %d > %d!", effectIndex, dspmem_chunk_bytes(*ch),
                  freeBytes);
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        int errorStatus;
        if (!mHwApi->uploadOwtEffect(ch->head.get(), dspmem_chunk_bytes(*ch),
                                     &mFfEffects[effectIndex], &effectIndex, &errorStatus)) {
            mStatsApi->logError(kHwApiError);
            ALOGE("Invalid uploadOwtEffect");
            return ndk::ScopedAStatus::fromExceptionCode(errorStatus);
        }
    } else if (effectIndex == WAVEFORM_SHORT_VIBRATION_EFFECT_INDEX ||
               effectIndex == WAVEFORM_LONG_VIBRATION_EFFECT_INDEX) {
        /* Update duration for long/short vibration. */
        mFfEffects[effectIndex].replay.length = static_cast<uint16_t>(timeoutMs);
        if (!mHwApi->setFFEffect(&mFfEffects[effectIndex], static_cast<uint16_t>(timeoutMs))) {
            mStatsApi->logError(kHwApiError);
            ALOGE("Failed to edit effect %d (%d): %s", effectIndex, errno, strerror(errno));
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
    }

    const std::scoped_lock<std::mutex> lock(mActiveId_mutex);
    mActiveId = effectIndex;
    /* Play the event now. */
    mStatsApi->logLatencyEnd();
    if (!mHwApi->setFFPlay(effectIndex, true)) {
        mStatsApi->logError(kHwApiError);
        ALOGE("Failed to play effect %d (%d): %s", effectIndex, errno, strerror(errno));
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    mAsyncHandle = std::async(&Vibrator::waitForComplete, this, callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setEffectAmplitude(float amplitude, float maximum) {
    HAPTICS_TRACE("setEffectAmplitude(amplitude:%f, maximum:%f)", amplitude, maximum);
    uint16_t scale = amplitudeToScale(amplitude, maximum);
    if (!mHwApi->setFFGain(scale)) {
        mStatsApi->logError(kHwApiError);
        ALOGE("Failed to set the gain to %u (%d): %s", scale, errno, strerror(errno));
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setGlobalAmplitude(bool set) {
    HAPTICS_TRACE("setGlobalAmplitude(set:%u)", set);
    uint8_t amplitude = set ? roundf(mLongEffectScale * mLongEffectVol[1]) : VOLTAGE_SCALE_MAX;
    if (!set) {
        mLongEffectScale = 1.0;  // Reset the scale for the later new effect.
    }
    return setEffectAmplitude(amplitude, VOLTAGE_SCALE_MAX);
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect> * /*_aidl_return*/) {
    mStatsApi->logError(kUnsupportedOpError);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t /*id*/, Effect /*effect*/,
                                            EffectStrength /*strength*/) {
    mStatsApi->logError(kUnsupportedOpError);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t /*id*/) {
    mStatsApi->logError(kUnsupportedOpError);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float *resonantFreqHz) {
    HAPTICS_TRACE("getResonantFrequency(resonantFreqHz)");
    *resonantFreqHz = mResonantFrequency;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getQFactor(float *qFactor) {
    HAPTICS_TRACE("getQFactor(qFactor)");
    std::string caldata{8, '0'};
    if (!mHwCal->getQ(&caldata)) {
        mStatsApi->logError(kHwCalError);
        ALOGE("Failed to get q factor (%d): %s", errno, strerror(errno));
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    *qFactor = static_cast<float>(std::stoul(caldata, nullptr, 16)) / (1 << Q16_BIT_SHIFT);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float *freqResolutionHz) {
    HAPTICS_TRACE("getFrequencyResolution(freqResolutionHz)");
    int32_t capabilities;
    Vibrator::getCapabilities(&capabilities);
    if (capabilities & IVibrator::CAP_FREQUENCY_CONTROL) {
        *freqResolutionHz = PWLE_FREQUENCY_RESOLUTION_HZ;
        return ndk::ScopedAStatus::ok();
    } else {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float *freqMinimumHz) {
    HAPTICS_TRACE("getFrequencyMinimum(freqMinimumHz)");
    int32_t capabilities;
    Vibrator::getCapabilities(&capabilities);
    if (capabilities & IVibrator::CAP_FREQUENCY_CONTROL) {
        *freqMinimumHz = PWLE_FREQUENCY_MIN_HZ;
        return ndk::ScopedAStatus::ok();
    } else {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
}

void Vibrator::createPwleMaxLevelLimitMap() {
    int32_t capabilities;
    Vibrator::getCapabilities(&capabilities);
    if (!(capabilities & IVibrator::CAP_FREQUENCY_CONTROL)) {
        mStatsApi->logError(kUnsupportedOpError);
        ALOGE("Frequency control not support.");
        return;
    }

    if (discretePwleMaxLevels.empty()) {
        mStatsApi->logError(kInitError);
        ALOGE("Discrete PWLE max level maps are empty.");
        return;
    }

    int32_t pwleMaxLevelLimitMapIdx = 0;
    std::map<float, float>::iterator itr0 = discretePwleMaxLevels.begin();
    if (discretePwleMaxLevels.size() == 1) {
        ALOGD("Discrete PWLE max level map size is 1");
        pwleMaxLevelLimitMapIdx =
                (itr0->first - PWLE_FREQUENCY_MIN_HZ) / PWLE_FREQUENCY_RESOLUTION_HZ;
        pwleMaxLevelLimitMap[pwleMaxLevelLimitMapIdx] = itr0->second;
        return;
    }

    auto itr1 = std::next(itr0, 1);

    while (itr1 != discretePwleMaxLevels.end()) {
        float x0 = itr0->first;
        float y0 = itr0->second;
        float x1 = itr1->first;
        float y1 = itr1->second;
        const float ratioOfXY = ((y1 - y0) / (x1 - x0));
        pwleMaxLevelLimitMapIdx =
                (itr0->first - PWLE_FREQUENCY_MIN_HZ) / PWLE_FREQUENCY_RESOLUTION_HZ;

        // FixLater: avoid floating point loop counters
        // NOLINTBEGIN(clang-analyzer-security.FloatLoopCounter,cert-flp30-c)
        for (float xp = x0; xp < (x1 + PWLE_FREQUENCY_RESOLUTION_HZ);
             xp += PWLE_FREQUENCY_RESOLUTION_HZ) {
            // NOLINTEND(clang-analyzer-security.FloatLoopCounter,cert-flp30-c)
            float yp = y0 + ratioOfXY * (xp - x0);

            pwleMaxLevelLimitMap[pwleMaxLevelLimitMapIdx++] = yp;
        }

        itr0++;
        itr1++;
    }
}

void Vibrator::createBandwidthAmplitudeMap() {
    // Use constant Q Factor of 10 from HW's suggestion
    const float qFactor = 10.0f;
    const float blSys = 1.1f;
    const float gravity = 9.81f;
    const float maxVoltage = 11.0f;
    float deviceMass = 0, locCoeff = 0;

    mHwCal->getDeviceMass(&deviceMass);
    mHwCal->getLocCoeff(&locCoeff);
    if (!deviceMass || !locCoeff) {
        mStatsApi->logError(kInitError);
        ALOGE("Failed to get Device Mass: %f and Loc Coeff: %f", deviceMass, locCoeff);
        return;
    }

    // Resistance value need to be retrieved from calibration file
    if (mRedc == 0.0) {
        std::string caldata{8, '0'};
        if (mHwCal->getRedc(&caldata)) {
            mHwApi->setRedc(caldata);
            mRedc = redcToFloat(&caldata);
        } else {
            mStatsApi->logError(kHwCalError);
            ALOGE("Failed to get resistance value from calibration file");
            return;
        }
    }

    std::vector<float> bandwidthAmplitudeMap(PWLE_BW_MAP_SIZE, 1.0);

    const float wnSys = mResonantFrequency * 2 * M_PI;
    const float powWnSys = pow(wnSys, 2);
    const float var2Para = wnSys / qFactor;

    float frequencyHz = PWLE_FREQUENCY_MIN_HZ;
    float frequencyRadians = 0.0f;
    float vLevel = 0.4473f;
    float vSys = (mLongEffectVol[1] / 100.0) * maxVoltage * vLevel;
    float maxAsys = 0;
    const float amplitudeSysPara = blSys * locCoeff / mRedc / deviceMass;

    for (int i = 0; i < PWLE_BW_MAP_SIZE; i++) {
        frequencyRadians = frequencyHz * 2 * M_PI;
        vLevel = pwleMaxLevelLimitMap[i];
        vSys = (mLongEffectVol[1] / 100.0) * maxVoltage * vLevel;

        float var1 = pow((powWnSys - pow(frequencyRadians, 2)), 2);
        float var2 = pow((var2Para * frequencyRadians), 2);

        float psysAbs = sqrt(var1 + var2);
        // The equation and all related details can be found in the bug
        float amplitudeSys =
                (vSys * amplitudeSysPara) * pow(frequencyRadians, 2) / psysAbs / gravity;
        // Record the maximum acceleration for the next for loop
        if (amplitudeSys > maxAsys)
            maxAsys = amplitudeSys;

        bandwidthAmplitudeMap[i] = amplitudeSys;
        frequencyHz += PWLE_FREQUENCY_RESOLUTION_HZ;
    }
    // Scaled the map between 0 and 1.0
    if (maxAsys > 0) {
        for (int j = 0; j < PWLE_BW_MAP_SIZE; j++) {
            bandwidthAmplitudeMap[j] =
                    std::floor((bandwidthAmplitudeMap[j] / maxAsys) * 1000) / 1000;
        }
        mBandwidthAmplitudeMap = bandwidthAmplitudeMap;
        mCreateBandwidthAmplitudeMapDone = true;
    } else {
        mCreateBandwidthAmplitudeMapDone = false;
    }
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float> *_aidl_return) {
    HAPTICS_TRACE("getBandwidthAmplitudeMap(_aidl_return)");
    int32_t capabilities;
    Vibrator::getCapabilities(&capabilities);
    if (capabilities & IVibrator::CAP_FREQUENCY_CONTROL) {
        if (!mCreateBandwidthAmplitudeMapDone) {
            createPwleMaxLevelLimitMap();
            createBandwidthAmplitudeMap();
        }
        *_aidl_return = mBandwidthAmplitudeMap;
        return (!mBandwidthAmplitudeMap.empty())
                       ? ndk::ScopedAStatus::ok()
                       : ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    } else {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t *durationMs) {
    HAPTICS_TRACE("getPwlePrimitiveDurationMax(durationMs)");
    int32_t capabilities;
    Vibrator::getCapabilities(&capabilities);
    if (capabilities & IVibrator::CAP_COMPOSE_PWLE_EFFECTS) {
        *durationMs = COMPOSE_PWLE_PRIMITIVE_DURATION_MAX_MS;
        return ndk::ScopedAStatus::ok();
    } else {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t *maxSize) {
    HAPTICS_TRACE("getPwleCompositionSizeMax(maxSize)");
    int32_t capabilities;
    Vibrator::getCapabilities(&capabilities);
    if (capabilities & IVibrator::CAP_COMPOSE_PWLE_EFFECTS) {
        *maxSize = COMPOSE_PWLE_SIZE_MAX_DEFAULT;
        return ndk::ScopedAStatus::ok();
    } else {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking> *supported) {
    HAPTICS_TRACE("getSupportedBraking(supported)");
    int32_t capabilities;
    Vibrator::getCapabilities(&capabilities);
    if (capabilities & IVibrator::CAP_COMPOSE_PWLE_EFFECTS) {
        *supported = {
                Braking::NONE,
        };
        return ndk::ScopedAStatus::ok();
    } else {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
}

static void resetPreviousEndAmplitudeEndFrequency(float *prevEndAmplitude,
                                                  float *prevEndFrequency) {
    HAPTICS_TRACE("     resetPreviousEndAmplitudeEndFrequency(prevEndAmplitude, prevEndFrequency)");
    const float reset = -1.0;
    *prevEndAmplitude = reset;
    *prevEndFrequency = reset;
}

static void incrementIndex(int *index) {
    *index += 1;
}

static void constructPwleSegment(dspmem_chunk *ch, uint16_t delay, uint16_t amplitude,
                                 uint16_t frequency, uint8_t flags, uint32_t vbemfTarget = 0) {
    HAPTICS_TRACE(
            "     constructPwleSegment(ch, delay:%u, amplitude:%u, frequency:%u, flags:%u"
            ", vbemfTarget:%u)",
            delay, amplitude, frequency, flags, vbemfTarget);
    dspmem_chunk_write(ch, 16, delay);
    dspmem_chunk_write(ch, 12, amplitude);
    dspmem_chunk_write(ch, 12, frequency);
    /* feature flags to control the chirp, CLAB braking, back EMF amplitude regulation */
    dspmem_chunk_write(ch, 8, (flags | 1) << 4);
    if (flags & PWLE_AMP_REG_BIT) {
        dspmem_chunk_write(ch, 24, vbemfTarget); /* target back EMF voltage */
    }
}

static int constructActiveSegment(dspmem_chunk *ch, int duration, float amplitude, float frequency,
                                  bool chirp) {
    HAPTICS_TRACE("     constructActiveSegment(ch, duration:%d, amplitude:%f, frequency:%f)",
                  duration, amplitude, frequency);
    uint16_t delay = 0;
    uint16_t amp = 0;
    uint16_t freq = 0;
    uint8_t flags = 0x0;
    if ((floatToUint16(duration, &delay, 4, 0.0f, COMPOSE_PWLE_PRIMITIVE_DURATION_MAX_MS) < 0) ||
        (floatToUint16(amplitude, &amp, 2048, CS40L26_PWLE_LEVEL_MIN, CS40L26_PWLE_LEVEL_MAX) <
         0) ||
        (floatToUint16(frequency, &freq, 4, PWLE_FREQUENCY_MIN_HZ, PWLE_FREQUENCY_MAX_HZ) < 0)) {
        ALOGE("Invalid argument: %d, %f, %f", duration, amplitude, frequency);
        return -ERANGE;
    }
    if (chirp) {
        flags |= PWLE_CHIRP_BIT;
    }
    constructPwleSegment(ch, delay, amp, freq, flags, 0 /*ignored*/);
    return 0;
}

static int constructBrakingSegment(dspmem_chunk *ch, int duration, Braking brakingType) {
    HAPTICS_TRACE("     constructBrakingSegment(ch, duration:%d, brakingType:%s)", duration,
                  toString(brakingType).c_str());
    uint16_t delay = 0;
    uint16_t freq = 0;
    uint8_t flags = 0x00;
    if (floatToUint16(duration, &delay, 4, 0.0f, COMPOSE_PWLE_PRIMITIVE_DURATION_MAX_MS) < 0) {
        ALOGE("Invalid argument: %d", duration);
        return -ERANGE;
    }
    floatToUint16(PWLE_FREQUENCY_MIN_HZ, &freq, 4, PWLE_FREQUENCY_MIN_HZ, PWLE_FREQUENCY_MAX_HZ);
    if (static_cast<std::underlying_type<Braking>::type>(brakingType)) {
        flags |= PWLE_BRAKE_BIT;
    }

    constructPwleSegment(ch, delay, 0 /*ignored*/, freq, flags, 0 /*ignored*/);
    return 0;
}

static void updateWLength(dspmem_chunk *ch, uint32_t totalDuration) {
    HAPTICS_TRACE("     updateWLength(ch, totalDuration:%u)", totalDuration);
    totalDuration *= 8;            /* Unit: 0.125 ms (since wlength played @ 8kHz). */
    totalDuration |= WT_LEN_CALCD; /* Bit 23 is for WT_LEN_CALCD; Bit 22 is for WT_INDEFINITE. */
    uint8_t *head = ch->head.get();
    *(head + 0) = (totalDuration >> 24) & 0xFF;
    *(head + 1) = (totalDuration >> 16) & 0xFF;
    *(head + 2) = (totalDuration >> 8) & 0xFF;
    *(head + 3) = totalDuration & 0xFF;
}

static void updateNSection(dspmem_chunk *ch, int segmentIdx) {
    HAPTICS_TRACE("     updateNSection(ch, segmentIdx:%u)", segmentIdx);
    uint8_t *head = ch->head.get();
    *(head + 7) |= (0xF0 & segmentIdx) >> 4; /* Bit 4 to 7 */
    *(head + 9) |= (0x0F & segmentIdx) << 4; /* Bit 3 to 0 */
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle> &composite,
                                         const std::shared_ptr<IVibratorCallback> &callback) {
    ATRACE_NAME(StringPrintf("Vibrator::composePwle size=%zu", composite.size()).c_str());
    HAPTICS_TRACE("composePwle(composite, callback)");
    int32_t capabilities;

    mStatsApi->logLatencyStart(kPwleEffectLatency);

    Vibrator::getCapabilities(&capabilities);
    if ((capabilities & IVibrator::CAP_COMPOSE_PWLE_EFFECTS) == 0) {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    if (composite.empty() || composite.size() > COMPOSE_PWLE_SIZE_MAX_DEFAULT) {
        mStatsApi->logError(kBadCompositeError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::vector<Braking> supported;
    Vibrator::getSupportedBraking(&supported);
    bool isClabSupported =
            std::find(supported.begin(), supported.end(), Braking::CLAB) != supported.end();

    int segmentIdx = 0;
    uint32_t totalDuration = 0;
    float prevEndAmplitude;
    float prevEndFrequency;
    resetPreviousEndAmplitudeEndFrequency(&prevEndAmplitude, &prevEndFrequency);
    dspmem_chunk ch(FF_CUSTOM_DATA_LEN_MAX_PWLE);
    bool chirp = false;

    dspmem_chunk_write(&ch, 24, 0x000000); /* Waveform length placeholder */
    dspmem_chunk_write(&ch, 8, 0);         /* Repeat */
    dspmem_chunk_write(&ch, 12, 0);        /* Wait time between repeats */
    dspmem_chunk_write(&ch, 8, 0x00);      /* nsections placeholder */

    for (auto &e : composite) {
        switch (e.getTag()) {
            case PrimitivePwle::active: {
                auto active = e.get<PrimitivePwle::active>();
                if (active.duration < 0 ||
                    active.duration > COMPOSE_PWLE_PRIMITIVE_DURATION_MAX_MS) {
                    mStatsApi->logError(kBadPrimitiveError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                }
                if (active.startAmplitude < PWLE_LEVEL_MIN ||
                    active.startAmplitude > PWLE_LEVEL_MAX ||
                    active.endAmplitude < PWLE_LEVEL_MIN || active.endAmplitude > PWLE_LEVEL_MAX) {
                    mStatsApi->logError(kBadPrimitiveError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                }
                if (active.startAmplitude > CS40L26_PWLE_LEVEL_MAX) {
                    active.startAmplitude = CS40L26_PWLE_LEVEL_MAX;
                }
                if (active.endAmplitude > CS40L26_PWLE_LEVEL_MAX) {
                    active.endAmplitude = CS40L26_PWLE_LEVEL_MAX;
                }

                if (active.startFrequency < PWLE_FREQUENCY_MIN_HZ ||
                    active.startFrequency > PWLE_FREQUENCY_MAX_HZ ||
                    active.endFrequency < PWLE_FREQUENCY_MIN_HZ ||
                    active.endFrequency > PWLE_FREQUENCY_MAX_HZ) {
                    mStatsApi->logError(kBadPrimitiveError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                }

                if (!((active.startAmplitude == prevEndAmplitude) &&
                      (active.startFrequency == prevEndFrequency))) {
                    if (constructActiveSegment(&ch, 0, active.startAmplitude, active.startFrequency,
                                               false) < 0) {
                        mStatsApi->logError(kPwleConstructionFailError);
                        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                    }
                    incrementIndex(&segmentIdx);
                }

                if (active.startFrequency != active.endFrequency) {
                    chirp = true;
                }
                if (constructActiveSegment(&ch, active.duration, active.endAmplitude,
                                           active.endFrequency, chirp) < 0) {
                    mStatsApi->logError(kPwleConstructionFailError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                }
                incrementIndex(&segmentIdx);

                prevEndAmplitude = active.endAmplitude;
                prevEndFrequency = active.endFrequency;
                totalDuration += active.duration;
                chirp = false;
                break;
            }
            case PrimitivePwle::braking: {
                auto braking = e.get<PrimitivePwle::braking>();
                if (braking.braking > Braking::CLAB) {
                    mStatsApi->logError(kBadPrimitiveError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                } else if (!isClabSupported && (braking.braking == Braking::CLAB)) {
                    mStatsApi->logError(kBadPrimitiveError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                }

                if (braking.duration > COMPOSE_PWLE_PRIMITIVE_DURATION_MAX_MS) {
                    mStatsApi->logError(kBadPrimitiveError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                }

                if (constructBrakingSegment(&ch, 0, braking.braking) < 0) {
                    mStatsApi->logError(kPwleConstructionFailError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                }
                incrementIndex(&segmentIdx);

                if (constructBrakingSegment(&ch, braking.duration, braking.braking) < 0) {
                    mStatsApi->logError(kPwleConstructionFailError);
                    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
                }
                incrementIndex(&segmentIdx);

                resetPreviousEndAmplitudeEndFrequency(&prevEndAmplitude, &prevEndFrequency);
                totalDuration += braking.duration;
                break;
            }
        }

        if (segmentIdx > COMPOSE_PWLE_SIZE_MAX_DEFAULT) {
            mStatsApi->logError(kPwleConstructionFailError);
            ALOGE("Too many PrimitivePwle section!");
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
    }
    dspmem_chunk_flush(&ch);

    /* Update wlength */
    totalDuration += MAX_COLD_START_LATENCY_MS;
    if (totalDuration > 0x7FFFF) {
        mStatsApi->logError(kPwleConstructionFailError);
        ALOGE("Total duration is too long (%d)!", totalDuration);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    } else {
        mFfEffects[WAVEFORM_PWLE].replay.length = totalDuration;
    }
    updateWLength(&ch, totalDuration);

    /* Update nsections */
    updateNSection(&ch, segmentIdx);

    return performEffect(WAVEFORM_MAX_INDEX /*ignored*/, VOLTAGE_SCALE_MAX /*ignored*/, &ch,
                         callback);
}

bool Vibrator::isUnderExternalControl() {
    HAPTICS_TRACE("isUnderExternalControl()");
    return mIsUnderExternalControl;
}

binder_status_t Vibrator::dump(int fd, const char **args, uint32_t numArgs) {
    HAPTICS_TRACE("dump(fd:%d, args, numArgs:%u)", fd, numArgs);
    if (fd < 0) {
        ALOGE("Called debug() with invalid fd.");
        return STATUS_OK;
    }

    (void)args;
    (void)numArgs;

    dprintf(fd, "AIDL:\n");

    dprintf(fd, "  F0 Offset: %" PRIu32 "\n", mF0Offset);

    dprintf(fd, "  Voltage Levels:\n");
    dprintf(fd, "    Tick Effect Min: %" PRIu32 " Max: %" PRIu32 "\n", mTickEffectVol[0],
            mTickEffectVol[1]);
    dprintf(fd, "    Click Effect Min: %" PRIu32 " Max: %" PRIu32 "\n", mClickEffectVol[0],
            mClickEffectVol[1]);
    dprintf(fd, "    Long Effect Min: %" PRIu32 " Max: %" PRIu32 "\n", mLongEffectVol[0],
            mLongEffectVol[1]);

    dprintf(fd, "  FF effect:\n");
    dprintf(fd, "    Physical waveform:\n");
    dprintf(fd, "\tId\tIndex\tt   ->\tt'\n");
    for (uint8_t effectId = 0; effectId < WAVEFORM_MAX_PHYSICAL_INDEX; effectId++) {
        dprintf(fd, "\t%d\t%d\t%d\t%d\n", mFfEffects[effectId].id,
                mFfEffects[effectId].u.periodic.custom_data[1], mEffectDurations[effectId],
                mFfEffects[effectId].replay.length);
    }
    dprintf(fd, "    OWT waveform:\n");
    dprintf(fd, "\tId\tBytes\tData\n");
    for (uint8_t effectId = WAVEFORM_MAX_PHYSICAL_INDEX; effectId < WAVEFORM_MAX_INDEX;
         effectId++) {
        uint32_t numBytes = mFfEffects[effectId].u.periodic.custom_len * 2;
        std::stringstream ss;
        ss << " ";
        for (int i = 0; i < numBytes; i++) {
            ss << std::uppercase << std::setfill('0') << std::setw(2) << std::hex
               << (uint16_t)(*(
                          reinterpret_cast<uint8_t *>(mFfEffects[effectId].u.periodic.custom_data) +
                          i))
               << " ";
        }
        dprintf(fd, "\t%d\t%d\t{%s}\n", mFfEffects[effectId].id, numBytes, ss.str().c_str());
    }

    dprintf(fd, "\n");
    dprintf(fd, "\n");

    mHwApi->debug(fd);

    dprintf(fd, "\n");

    mHwCal->debug(fd);

    dprintf(fd, "\n");

    mStatsApi->debug(fd);

    fsync(fd);
    return STATUS_OK;
}

bool Vibrator::hasHapticAlsaDevice() {
    HAPTICS_TRACE("hasHapticAlsaDevice()");
    // We need to call findHapticAlsaDevice once only. Calling in the
    // constructor is too early in the boot process and the pcm file contents
    // are empty. Hence we make the call here once only right before we need to.
    if (!mConfigHapticAlsaDeviceDone) {
        if (mHwApi->getHapticAlsaDevice(&mCard, &mDevice)) {
            mHasHapticAlsaDevice = true;
            mConfigHapticAlsaDeviceDone = true;
        } else {
            mStatsApi->logError(kAlsaFailError);
            ALOGE("Haptic ALSA device not supported");
        }
    } else {
        ALOGD("Haptic ALSA device configuration done.");
    }
    return mHasHapticAlsaDevice;
}

ndk::ScopedAStatus Vibrator::getSimpleDetails(Effect effect, EffectStrength strength,
                                              uint32_t *outEffectIndex, uint32_t *outTimeMs,
                                              uint32_t *outVolLevel) {
    HAPTICS_TRACE(
            "getSimpleDetails(effect:%s, strength:%s, outEffectIndex, outTimeMs"
            ", outVolLevel)",
            toString(effect).c_str(), toString(strength).c_str());
    uint32_t effectIndex;
    uint32_t timeMs;
    float intensity;
    uint32_t volLevel;
    switch (strength) {
        case EffectStrength::LIGHT:
            intensity = 0.5f;
            break;
        case EffectStrength::MEDIUM:
            intensity = 0.7f;
            break;
        case EffectStrength::STRONG:
            intensity = 1.0f;
            break;
        default:
            mStatsApi->logError(kUnsupportedOpError);
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    switch (effect) {
        case Effect::TEXTURE_TICK:
            effectIndex = WAVEFORM_LIGHT_TICK_INDEX;
            intensity *= 0.5f;
            break;
        case Effect::TICK:
            effectIndex = WAVEFORM_CLICK_INDEX;
            intensity *= 0.5f;
            break;
        case Effect::CLICK:
            effectIndex = WAVEFORM_CLICK_INDEX;
            intensity *= 0.7f;
            break;
        case Effect::HEAVY_CLICK:
            effectIndex = WAVEFORM_CLICK_INDEX;
            intensity *= 1.0f;
            break;
        default:
            mStatsApi->logError(kUnsupportedOpError);
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    volLevel = intensityToVolLevel(intensity, effectIndex);
    timeMs = mEffectDurations[effectIndex] + MAX_COLD_START_LATENCY_MS;

    *outEffectIndex = effectIndex;
    *outTimeMs = timeMs;
    *outVolLevel = volLevel;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompoundDetails(Effect effect, EffectStrength strength,
                                                uint32_t *outTimeMs, dspmem_chunk *outCh) {
    HAPTICS_TRACE("getCompoundDetails(effect:%s, strength:%s, outTimeMs, outCh)",
                  toString(effect).c_str(), toString(strength).c_str());
    ndk::ScopedAStatus status;
    uint32_t timeMs = 0;
    uint32_t thisEffectIndex;
    uint32_t thisTimeMs;
    uint32_t thisVolLevel;
    switch (effect) {
        case Effect::DOUBLE_CLICK:
            dspmem_chunk_write(outCh, 8, 0); /* Padding */
            dspmem_chunk_write(outCh, 8, 2); /* nsections */
            dspmem_chunk_write(outCh, 8, 0); /* repeat */

            status = getSimpleDetails(Effect::CLICK, strength, &thisEffectIndex, &thisTimeMs,
                                      &thisVolLevel);
            if (!status.isOk()) {
                mStatsApi->logError(kBadEffectError);
                return status;
            }
            timeMs += thisTimeMs;

            dspmem_chunk_write(outCh, 8, (uint8_t)(0xFF & thisVolLevel));    /* amplitude */
            dspmem_chunk_write(outCh, 8, (uint8_t)(0xFF & thisEffectIndex)); /* index */
            dspmem_chunk_write(outCh, 8, 0);                                 /* repeat */
            dspmem_chunk_write(outCh, 8, 0);                                 /* flags */
            dspmem_chunk_write(outCh, 16,
                               (uint16_t)(0xFFFF & WAVEFORM_DOUBLE_CLICK_SILENCE_MS)); /* delay */

            timeMs += WAVEFORM_DOUBLE_CLICK_SILENCE_MS + MAX_PAUSE_TIMING_ERROR_MS;

            status = getSimpleDetails(Effect::HEAVY_CLICK, strength, &thisEffectIndex, &thisTimeMs,
                                      &thisVolLevel);
            if (!status.isOk()) {
                mStatsApi->logError(kBadEffectError);
                return status;
            }
            timeMs += thisTimeMs;

            dspmem_chunk_write(outCh, 8, (uint8_t)(0xFF & thisVolLevel));    /* amplitude */
            dspmem_chunk_write(outCh, 8, (uint8_t)(0xFF & thisEffectIndex)); /* index */
            dspmem_chunk_write(outCh, 8, 0);                                 /* repeat */
            dspmem_chunk_write(outCh, 8, 0);                                 /* flags */
            dspmem_chunk_write(outCh, 16, 0);                                /* delay */
            dspmem_chunk_flush(outCh);

            break;
        default:
            mStatsApi->logError(kUnsupportedOpError);
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    *outTimeMs = timeMs;
    mFfEffects[WAVEFORM_COMPOSE].replay.length = static_cast<uint16_t>(timeMs);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDetails(CompositePrimitive primitive,
                                                 uint32_t *outEffectIndex) {
    HAPTICS_TRACE("getPrimitiveDetails(primitive:%s, outEffectIndex)", toString(primitive).c_str());
    uint32_t effectIndex;
    uint32_t primitiveBit = 1 << int32_t(primitive);
    if ((primitiveBit & mSupportedPrimitivesBits) == 0x0) {
        mStatsApi->logError(kUnsupportedOpError);
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    switch (primitive) {
        case CompositePrimitive::NOOP:
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        case CompositePrimitive::CLICK:
            effectIndex = WAVEFORM_CLICK_INDEX;
            break;
        case CompositePrimitive::THUD:
            effectIndex = WAVEFORM_THUD_INDEX;
            break;
        case CompositePrimitive::SPIN:
            effectIndex = WAVEFORM_SPIN_INDEX;
            break;
        case CompositePrimitive::QUICK_RISE:
            effectIndex = WAVEFORM_QUICK_RISE_INDEX;
            break;
        case CompositePrimitive::SLOW_RISE:
            effectIndex = WAVEFORM_SLOW_RISE_INDEX;
            break;
        case CompositePrimitive::QUICK_FALL:
            effectIndex = WAVEFORM_QUICK_FALL_INDEX;
            break;
        case CompositePrimitive::LIGHT_TICK:
            effectIndex = WAVEFORM_LIGHT_TICK_INDEX;
            break;
        case CompositePrimitive::LOW_TICK:
            effectIndex = WAVEFORM_LOW_TICK_INDEX;
            break;
        default:
            mStatsApi->logError(kUnsupportedOpError);
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    *outEffectIndex = effectIndex;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::performEffect(Effect effect, EffectStrength strength,
                                           const std::shared_ptr<IVibratorCallback> &callback,
                                           int32_t *outTimeMs) {
    HAPTICS_TRACE("performEffect(effect:%s, strength:%s, callback, outTimeMs)",
                  toString(effect).c_str(), toString(strength).c_str());
    ndk::ScopedAStatus status;
    uint32_t effectIndex;
    uint32_t timeMs = 0;
    uint32_t volLevel;
    std::optional<dspmem_chunk> maybeCh;
    switch (effect) {
        case Effect::TEXTURE_TICK:
            // fall-through
        case Effect::TICK:
            // fall-through
        case Effect::CLICK:
            // fall-through
        case Effect::HEAVY_CLICK:
            status = getSimpleDetails(effect, strength, &effectIndex, &timeMs, &volLevel);
            break;
        case Effect::DOUBLE_CLICK:
            maybeCh.emplace(FF_CUSTOM_DATA_LEN_MAX_COMP);
            status = getCompoundDetails(effect, strength, &timeMs, &*maybeCh);
            volLevel = VOLTAGE_SCALE_MAX;
            break;
        default:
            mStatsApi->logError(kUnsupportedOpError);
            status = ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
            break;
    }
    if (status.isOk()) {
        dspmem_chunk *ch = maybeCh ? &*maybeCh : nullptr;
        status = performEffect(effectIndex, volLevel, ch, callback);
    }

    *outTimeMs = timeMs;
    return status;
}

ndk::ScopedAStatus Vibrator::performEffect(uint32_t effectIndex, uint32_t volLevel,
                                           dspmem_chunk *ch,
                                           const std::shared_ptr<IVibratorCallback> &callback) {
    HAPTICS_TRACE("performEffect(effectIndex:%u, volLevel:%u, ch, callback)", effectIndex,
                  volLevel);
    setEffectAmplitude(volLevel, VOLTAGE_SCALE_MAX);
    return on(MAX_TIME_MS, effectIndex, ch, callback);
}

void Vibrator::waitForComplete(std::shared_ptr<IVibratorCallback> &&callback) {
    HAPTICS_TRACE("waitForComplete(callback)");

    if (!mHwApi->pollVibeState(VIBE_STATE_HAPTIC, POLLING_TIMEOUT)) {
        ALOGW("Failed to get state \"Haptic\"");
    }
    ATRACE_BEGIN("Vibrating");
    mHwApi->pollVibeState(VIBE_STATE_STOPPED);
    ATRACE_END();

    const std::scoped_lock<std::mutex> lock(mActiveId_mutex);
    uint32_t effectCount = WAVEFORM_MAX_PHYSICAL_INDEX;
    if ((mActiveId >= WAVEFORM_MAX_PHYSICAL_INDEX) &&
        (!mHwApi->eraseOwtEffect(mActiveId, &mFfEffects))) {
        mStatsApi->logError(kHwApiError);
        ALOGE("Failed to clean up the composed effect %d", mActiveId);
    } else {
        ALOGD("waitForComplete: Vibrator is already off");
    }
    mHwApi->getEffectCount(&effectCount);
    // Do waveform number checking
    if ((effectCount > WAVEFORM_MAX_PHYSICAL_INDEX) &&
        (!mHwApi->eraseOwtEffect(WAVEFORM_MAX_INDEX, &mFfEffects))) {
        mStatsApi->logError(kHwApiError);
        ALOGE("Failed to forcibly clean up all composed effect");
    }

    mActiveId = -1;

    if (callback) {
        auto ret = callback->onComplete();
        if (!ret.isOk()) {
            ALOGE("Failed completion callback: %d", ret.getExceptionCode());
        }
    }
}

uint32_t Vibrator::intensityToVolLevel(float intensity, uint32_t effectIndex) {
    HAPTICS_TRACE("intensityToVolLevel(intensity:%f, effectIndex:%u)", intensity, effectIndex);

    uint32_t volLevel;
    auto calc = [](float intst, std::array<uint32_t, 2> v) -> uint32_t {
        return std::lround(intst * (v[1] - v[0])) + v[0];
    };

    switch (effectIndex) {
        case WAVEFORM_LIGHT_TICK_INDEX:
            volLevel = calc(intensity, mTickEffectVol);
            break;
        case WAVEFORM_QUICK_RISE_INDEX:
            // fall-through
        case WAVEFORM_QUICK_FALL_INDEX:
            volLevel = calc(intensity, mLongEffectVol);
            break;
        case WAVEFORM_CLICK_INDEX:
            // fall-through
        case WAVEFORM_THUD_INDEX:
            // fall-through
        case WAVEFORM_SPIN_INDEX:
            // fall-through
        case WAVEFORM_SLOW_RISE_INDEX:
            // fall-through
        default:
            volLevel = calc(intensity, mClickEffectVol);
            break;
    }
    return volLevel;
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
