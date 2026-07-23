/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "HalProxyCallback.h"

#include <cinttypes>
#include <mutex>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_0 {
namespace implementation {

static constexpr int32_t kBitsAfterSubHalIndex = 24;

// NEW: Proximity rate limiting globals
static std::mutex gProximityMutex;
static int64_t gLastProximityEventTime = 0;
static float gLastProximityValue = -1.0f;
static constexpr int64_t kProximityEventMinIntervalNs = 500000000LL; // 500ms

/**
 * Set the subhal index as first byte of sensor handle and return this modified version.
 *
 * @param sensorHandle The sensor handle to modify.
 * @param subHalIndex The index in the hal proxy of the sub hal this sensor belongs to.
 *
 * @return The modified sensor handle.
 */
int32_t setSubHalIndex(int32_t sensorHandle, size_t subHalIndex) {
    return sensorHandle | (static_cast<int32_t>(subHalIndex) << kBitsAfterSubHalIndex);
}

void HalProxyCallbackBase::postEvents(const std::vector<V2_1::Event>& events,
                                      ScopedWakelock wakelock) {
    if (events.empty() || !mCallback->areThreadsRunning()) return;
    size_t numWakeupEvents;
    std::vector<V2_1::Event> processedEvents = processEvents(events, &numWakeupEvents);
    if (numWakeupEvents > 0) {
        ALOG_ASSERT(wakelock.isLocked(),
                    "Wakeup events posted while wakelock unlocked for subhal"
                    " w/ index %" PRId32 ".",
                    mSubHalIndex);
    } else {
        ALOG_ASSERT(!wakelock.isLocked(),
                    "No Wakeup events posted but wakelock locked for subhal"
                    " w/ index %" PRId32 ".",
                    mSubHalIndex);
    }
    mCallback->postEventsToMessageQueue(processedEvents, numWakeupEvents, std::move(wakelock));
}

ScopedWakelock HalProxyCallbackBase::createScopedWakelock(bool lock) {
    ScopedWakelock wakelock(mRefCounter, lock);
    return wakelock;
}

std::vector<V2_1::Event> HalProxyCallbackBase::processEvents(const std::vector<V2_1::Event>& events,
                                                             size_t* numWakeupEvents) const {
    *numWakeupEvents = 0;
    std::vector<V2_1::Event> eventsOut;
    for (V2_1::Event event : events) {
        event.sensorHandle = setSubHalIndex(event.sensorHandle, mSubHalIndex);
        if (event.sensorType == V2_1::SensorType::DYNAMIC_SENSOR_META) {
            event.u.dynamic.sensorHandle =
                    setSubHalIndex(event.u.dynamic.sensorHandle, mSubHalIndex);
        }
        
        const V2_1::SensorInfo& sensor = mCallback->getSensorInfo(event.sensorHandle);
        
        // NEW: Rate-limit proximity events
        if (sensor.type == V2_1::SensorType::PROXIMITY) {
            std::lock_guard<std::mutex> lock(gProximityMutex);
            int64_t now = getTimeNow();
            
            // Skip if too frequent
            if (now - gLastProximityEventTime < kProximityEventMinIntervalNs) {
                ALOGV("Dropping proximity event - rate limited");
                continue;
            }
            
            // Skip if value hasn't changed
            if (event.u.scalar == gLastProximityValue && gLastProximityValue >= 0) {
                ALOGV("Dropping proximity event - no change");
                continue;
            }
            
            gLastProximityEventTime = now;
            gLastProximityValue = event.u.scalar;
        }
        
        eventsOut.push_back(event);
        if ((sensor.flags & V1_0::SensorFlagBits::WAKE_UP) != 0) {
            (*numWakeupEvents)++;
        }
    }
    return eventsOut;
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace sensors
}  // namespace hardware
}  // namespace android
