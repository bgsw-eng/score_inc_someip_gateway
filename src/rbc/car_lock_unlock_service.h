/********************************************************************************
 * Copyright (c) 2025 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

#ifndef SRC_RBC_CAR_LOCK_UNLOCK_SERVICE
#define SRC_RBC_CAR_LOCK_UNLOCK_SERVICE

#include <cstdint>

#include "score/mw/com/types.h"

// Macro to define a single-event service interface and its skeleton alias.
// All generated interfaces expose a uniform `event_` member so the gateway
// dispatcher can forward payloads without knowing the concrete event type.
// Usage:
//   DEFINE_RBC_SINGLE_EVENT_SERVICE(MyEvent, MyDataStruct, "event_string_name")
// Generates: MyEventInterface<Trait>  and  MyEventSkeleton
#define DEFINE_RBC_SINGLE_EVENT_SERVICE(BaseName, DataType, EventNameStr)     \
    template <typename Trait>                                                 \
    class BaseName##Interface : public Trait::Base {                          \
       public:                                                                \
        using Trait::Base::Base;                                              \
        typename Trait::template Event<DataType> event_{*this, EventNameStr}; \
    };                                                                        \
    using BaseName##Skeleton = score::mw::com::AsSkeleton<BaseName##Interface>;

namespace rbc_service {

// ---------------------------------------------------------------------------
// Event: car_lock_unlock_status
struct CarLockUnlockStatus {
    uint8_t status;  // 0 = unlocked, 1 = locked
};
DEFINE_RBC_SINGLE_EVENT_SERVICE(CarLockUnlockStatus, CarLockUnlockStatus, "car_lock_unlock_status")

// ---------------------------------------------------------------------------
// Event: mu1_reserved02 — Hazard lamp on/off status (Service 0x3003, Event 0x8003)
struct HazardLampStatus {
    uint8_t status;  // 0 = OFF, 1 = ON
};
DEFINE_RBC_SINGLE_EVENT_SERVICE(HazardLampStatus, HazardLampStatus, "mu1_reserved02")

// ---------------------------------------------------------------------------
// Event: mu1_reserved03 — Position lamp on/off status (Service 0x3003, Event 0x8004)
struct PositionLampStatus {
    uint8_t status;  // 0 = OFF, 1 = ON
};
DEFINE_RBC_SINGLE_EVENT_SERVICE(PositionLampStatus, PositionLampStatus, "mu1_reserved03")

// ---------------------------------------------------------------------------
// Event: mu2_reserved01 — Approach lamp on/off status (Service 0x3004, Event 0x8009)
struct ApproachLampStatus {
    uint8_t status;  // 0 = OFF, 1 = ON
};
DEFINE_RBC_SINGLE_EVENT_SERVICE(ApproachLampStatus, ApproachLampStatus, "mu2_reserved01")

// ---------------------------------------------------------------------------
// Event: RBC_IH_Approach_Lamp_On_Off_Cmd — Approach lamp on/off command (Service 0x4004, Method
// 0x8002)
struct ApproachLampCommand {
    uint8_t command;  // 0 = OFF, 1 = ON
};
DEFINE_RBC_SINGLE_EVENT_SERVICE(ApproachLampCommand, ApproachLampCommand,
                                "RBC_IH_Approach_Lamp_On_Off_Cmd")

// ---------------------------------------------------------------------------
// Event: RBC_IH_Hazard_Lamp_On_Off_Cmd — Hazard lamp on/off command (Service 0x4004, Method 0x8001)
struct HazardLampCommand {
    uint8_t command;  // 0 = OFF, 1 = ON
};
DEFINE_RBC_SINGLE_EVENT_SERVICE(HazardLampCommand, HazardLampCommand,
                                "RBC_IH_Hazard_Lamp_On_Off_Cmd")

// ---------------------------------------------------------------------------
// Event: RBC_IH_Car_Lock_Unlock_Cmd — Car lock/unlock command (Service 0x4003, Method 0x8001)
struct CarLockUnlockCommand {
    uint8_t command;  // 0 = UNLOCK, 1 = LOCK
};
DEFINE_RBC_SINGLE_EVENT_SERVICE(CarLockUnlockCommand, CarLockUnlockCommand,
                                "rbc_car_lock_unlock_cmd")

// ---------------------------------------------------------------------------
// Event: vehicle_speed_data — Vehicle speed in km/h (Service 0x1001, Event 0x1002)
struct VehicleSpeed {
    float speed_kmh;  // Vehicle speed in km/h
};
DEFINE_RBC_SINGLE_EVENT_SERVICE(VehicleSpeed, VehicleSpeed, "vehicle_speed_data")

}  // namespace rbc_service

#endif  // SRC_RBC_CAR_LOCK_UNLOCK_SERVICE
