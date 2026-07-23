/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "android.hardware.sensors@2.1-service"

#include <android/hardware/sensors/2.1/ISensors.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>
#include <utils/StrongPointer.h>
#include "HalProxy.h"

#include <cutils/uevent.h>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>

using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::sensors::V2_1::ISensors;
using android::hardware::sensors::V2_1::implementation::HalProxyV2_1;

// NEW: Suspend monitor thread
static void* suspend_monitor_thread(void* arg) {
    HalProxyV2_1* hal = static_cast<HalProxyV2_1*>(arg);
    int fd = uevent_open_socket(64 * 1024, true);
    if (fd < 0) {
        ALOGE("Failed to create uevent socket: %s", strerror(errno));
        return nullptr;
    }
    
    struct pollfd pfd = {fd, POLLIN, 0};
    char buffer[1024];
    
    ALOGD("Sensor suspend monitor thread started");
    
    while (true) {
        int ret = poll(&pfd, 1, -1);
        if (ret <= 0) {
            if (ret < 0 && errno != EINTR) {
                ALOGE("poll failed: %s", strerror(errno));
            }
            continue;
        }
        
        ret = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (ret <= 0) {
            if (ret < 0 && errno != EINTR) {
                ALOGE("recv failed: %s", strerror(errno));
            }
            continue;
        }
        buffer[ret] = '\0';
        
        if (strstr(buffer, "SUBSYSTEM=power")) {
            if (strstr(buffer, "SUSPEND=1")) {
                ALOGD("System suspending - releasing sensor wakelocks");
                hal->onSuspend();
            } else if (strstr(buffer, "RESUME=1")) {
                ALOGD("System resuming");
                hal->onResume();
            }
        }
    }
    
    close(fd);
    return nullptr;
}

int main(int /* argc */, char** /* argv */) {
    configureRpcThreadpool(1, true);

    HalProxyV2_1* sensors = new HalProxyV2_1();
    
    // Register as HIDL service
    android::sp<android::hardware::sensors::V2_1::ISensors> service = sensors;
    if (service->registerAsService() != ::android::OK) {
        ALOGE("Failed to register Sensors HAL instance");
        return -1;
    }

    // NEW: Start suspend monitor thread
    pthread_t thread;
    pthread_create(&thread, nullptr, suspend_monitor_thread, sensors);
    pthread_detach(thread);
    
    ALOGI("Sensor HAL service started with suspend monitoring");

    joinRpcThreadpool();
    return 1;  // joinRpcThreadpool shouldn't exit
}
