#pragma once

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

#include <inttypes.h>

#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <vector>

#include "CpuFrequencyReader.h"
#include "ICpuLoadReader.h"
#include "WorkDurationProcessor.h"

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace impl {
namespace pixel {

constexpr uint32_t kNumCpuPolicies = 3;

enum class ThrottleDecision {
    NO_THROTTLE = 0,
    THROTTLE_60 = 1,
    THROTTLE_70 = 2,
    THROTTLE_80 = 3,
    THROTTLE_90 = 4
};

struct ModelInput {
    std::array<double, kNumCpuPolicies> cpuPolicyAverageFrequencyHz;
    std::array<double, NUM_CPU_CORES> cpuCoreIdleTimesPercentage;
    WorkDurationFeatures workDurationFeatures;
    ThrottleDecision previousThrottleDecision;

    bool SetCpuFreqiencies(
            const std::vector<CpuPolicyAverageFrequency> &cpuPolicyAverageFrequencies);

    void LogToAtrace() const;

    bool operator==(const ModelInput &other) const {
        return cpuPolicyAverageFrequencyHz == other.cpuPolicyAverageFrequencyHz &&
               cpuCoreIdleTimesPercentage == other.cpuCoreIdleTimesPercentage &&
               workDurationFeatures == other.workDurationFeatures &&
               previousThrottleDecision == other.previousThrottleDecision;
    }
};

ThrottleDecision RunModel(const std::deque<ModelInput> &modelInputs);

std::string ThrottleString(ThrottleDecision throttleDecision);

}  // namespace pixel
}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace google
}  // namespace aidl
