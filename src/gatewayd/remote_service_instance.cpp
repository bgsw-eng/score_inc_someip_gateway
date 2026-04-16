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

#include "remote_service_instance.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <type_traits>

#include "score/mw/com/types.h"

using score::mw::com::GenericProxy;
using score::mw::com::SamplePtr;

namespace score::someip_gateway::gatewayd {

using network_service::interfaces::message_transfer::SomeipMessageTransferProxy;

static const std::size_t max_sample_count = 10;
static const std::size_t SOMEIP_FULL_HEADER_SIZE = 16;

// Shared state for RBC signals (thread-safe JSON file writing)
static std::recursive_mutex rbc_signals_mutex;
static std::map<std::string, uint32_t> rbc_signals_cache;

// RBC signal mappings (received from SOME/IP, forwarded to IPC skeletons)
// scoreApp reads these from /tmp/rbc_signals.json and writes to external databroker via Velocitas
// SDK LOCK STATUS   (0x3003/0x8002) → MU1_Reserved_01 HAZARD LAMP   (0x3003/0x8003) →
// MU1_Reserved_02 POSITION LAMP (0x3003/0x8004) → MU1_Reserved_03 APPROACH LAMP (0x3004/0x8009) →
// MU2_Reserved01

// Helper function to write RBC signals to JSON file (read by scoreApp)
static void writeRbcSignalsJson() {
    // Write all cached RBC signals to JSON file atomically
    std::lock_guard<std::recursive_mutex> lock(rbc_signals_mutex);

    // Build JSON string manually (avoids external dependencies)
    std::string json = "{\n  \"rbc_signals\": {\n";
    bool first = true;
    for (const auto& [key, value] : rbc_signals_cache) {
        if (!first) json += ",\n";
        json += "    \"" + key + "\": " + std::to_string(value);
        first = false;
    }
    json += "\n  }\n}\n";

    // Write to temporary file, then atomic rename
    const char* json_file = "/tmp/rbc_signals.json";
    const char* tmp_file = "/tmp/rbc_signals.json.tmp";

    std::ofstream out(tmp_file);
    if (out.is_open()) {
        out << json;
        out.close();
        // Atomic rename (mv on Linux is atomic)
        rename(tmp_file, json_file);

        std::cout << "[gatewayd-json] Updated " << json_file << " with " << rbc_signals_cache.size()
                  << " signals" << std::endl;
    } else {
        std::cerr << "[gatewayd-json] Failed to write " << tmp_file << std::endl;
    }
}

static void writeRbcToDatabroker(uint16_t svc_id, uint16_t evt_id,
                                 const score::cpp::span<const std::byte>& payload,
                                 const char* signal_name) {
    // Extract value from SOME/IP payload (typically 1 byte)
    if (payload.empty()) {
        return;
    }
    uint32_t value = static_cast<uint32_t>(static_cast<uint8_t>(payload.data()[0]));
    std::string vss_path;
    std::string signal_key;

    // Map SOME/IP service:event to signal key and VSS path
    if (svc_id == 0x3003 && evt_id == 0x8002) {
        signal_key = "LOCK_STATUS";
        vss_path = "Vehicle.Powertrain.ElectricMotor.MU1_Reserved_01";
    } else if (svc_id == 0x3003 && evt_id == 0x8003) {
        signal_key = "HAZARD_LAMP";
        vss_path = "Vehicle.Powertrain.ElectricMotor.MU1_Reserved_02";
    } else if (svc_id == 0x3003 && evt_id == 0x8004) {
        signal_key = "POSITION_LAMP";
        vss_path = "Vehicle.Powertrain.ElectricMotor.MU1_Reserved_03";
    } else if (svc_id == 0x3004 && evt_id == 0x8009) {
        signal_key = "APPROACH_LAMP";
        vss_path = "Vehicle.Powertrain.TractionBattery.DTE.MU2_Reserved01";
    } else {
        return;  // Not an RBC signal
    }

    // Update cache and write to JSON file
    {
        std::lock_guard<std::recursive_mutex> lock(rbc_signals_mutex);
        bool changed = (rbc_signals_cache[signal_key] != value);
        rbc_signals_cache[signal_key] = value;

        std::cout << "[gatewayd] " << signal_name << " (" << vss_path << ") = " << value
                  << std::endl;
        // Always write full JSON to ensure scoreApp has all signals
        writeRbcSignalsJson();
    }
}

RemoteServiceInstance::RemoteServiceInstance(
    std::shared_ptr<const config::ServiceInstance> service_instance_config,
    IpcSkeleton&& ipc_skeleton, SomeipMessageTransferProxy someip_message_proxy)
    : service_instance_config_(std::move(service_instance_config)),
      ipc_skeleton_(std::move(ipc_skeleton)),
      someip_message_proxy_(std::move(someip_message_proxy)) {
    // TODO: Error handling
    std::visit([](auto& skel) { (void)skel.OfferService(); }, ipc_skeleton_);

    // TODO: This should be dispatched centrally
    someip_message_proxy_.message_.SetReceiveHandler([this]() {
        someip_message_proxy_.message_.GetNewSamples(
            [this](auto message_sample) {
                // TODO: Check if size is larger than capacity of data
                score::cpp::span<const std::byte> message(message_sample->data,
                                                          message_sample->size);
                if (message.size() < SOMEIP_FULL_HEADER_SIZE) {
                    std::cerr << "Received SOME/IP message is too small: " << message.size()
                              << " bytes." << std::endl;
                    return;
                }
                // Extract service/event IDs from header for filtering
                uint16_t svc_id =
                    (static_cast<uint16_t>(message[0]) << 8) | static_cast<uint16_t>(message[1]);
                uint16_t evt_id =
                    (static_cast<uint16_t>(message[2]) << 8) | static_cast<uint16_t>(message[3]);

                // Filter: only process messages for RBC services (0x3003, 0x3004)
                if (!((svc_id == 0x3003 &&
                       (evt_id == 0x8002 || evt_id == 0x8003 || evt_id == 0x8004)) ||
                      (svc_id == 0x3004 && evt_id == 0x8009))) {
                    // Not an RBC message, skip processing
                    return;
                }

                // TODO: Check service id, method id, etc. Maybe do that in the dispatcher already?
                auto payload = message.subspan(SOMEIP_FULL_HEADER_SIZE);

                const char* signal_name = "UNKNOWN";

                if (svc_id == 0x3003 && evt_id == 0x8002) {
                    signal_name = "LOCK STATUS";
                } else if (svc_id == 0x3003 && evt_id == 0x8003) {
                    signal_name = "HAZARD LAMP";
                } else if (svc_id == 0x3003 && evt_id == 0x8004) {
                    signal_name = "POSITION LAMP";
                } else if (svc_id == 0x3004 && evt_id == 0x8009) {
                    signal_name = "APPROACH LAMP";
                }

                // TODO: deserialization
                std::visit(
                    [payload, signal_name, svc_id, evt_id](auto& skel) {
                        using SkeletonT = std::decay_t<decltype(skel)>;
                        if constexpr (std::is_same_v<SkeletonT,
                                                     echo_service::EchoResponseSkeleton>) {
                            auto maybe_sample = skel.echo_response_tiny_.Allocate();
                            if (!maybe_sample.has_value()) {
                                std::cerr << "Failed to allocate SOME/IP message:"
                                          << maybe_sample.error().Message() << std::endl;
                                return;
                            }
                            auto sample = std::move(maybe_sample).value();
                            std::memcpy(
                                sample.Get(), payload.data(),
                                std::min(sizeof(echo_service::EchoResponseTiny), payload.size()));
                            skel.echo_response_tiny_.Send(std::move(sample));
                        } else {
                            // Generic path: all RBC single-event skeletons expose
                            // a uniform `event_` member via DEFINE_RBC_SINGLE_EVENT_SERVICE.
                            auto maybe_sample = skel.event_.Allocate();
                            if (!maybe_sample.has_value()) {
                                std::cerr << "Failed to allocate SOME/IP message:"
                                          << maybe_sample.error().Message() << std::endl;
                                return;
                            }
                            auto sample = std::move(maybe_sample).value();
                            using DataT = typename std::decay_t<decltype(*sample.Get())>;
                            std::memcpy(sample.Get(), payload.data(),
                                        std::min(sizeof(DataT), payload.size()));
                            std::cout
                                << "[gatewayd] " << signal_name
                                << " forwarded to IPC skeleton, value="
                                << (payload.empty()
                                        ? -1
                                        : static_cast<int>(static_cast<uint8_t>(payload.data()[0])))
                                << std::endl;
                            skel.event_.Send(std::move(sample));

                            // Also write RBC signals directly to databroker for scoreApp
                            writeRbcToDatabroker(svc_id, evt_id, payload, signal_name);
                        }
                    },
                    ipc_skeleton_);
            },
            max_sample_count);
    });

    someip_message_proxy_.message_.Subscribe(max_sample_count);
}

namespace {
struct FindServiceContext {
    std::shared_ptr<const config::ServiceInstance> config;
    RemoteServiceInstance::IpcSkeleton skeleton;
    std::vector<std::unique_ptr<RemoteServiceInstance>>& instances;

    FindServiceContext(std::shared_ptr<const config::ServiceInstance> config_,
                       RemoteServiceInstance::IpcSkeleton&& skeleton_,
                       std::vector<std::unique_ptr<RemoteServiceInstance>>& instances_)
        : config(std::move(config_)), skeleton(std::move(skeleton_)), instances(instances_) {}
};

}  // namespace

Result<mw::com::FindServiceHandle> RemoteServiceInstance::CreateAsyncRemoteService(
    std::shared_ptr<const config::ServiceInstance> service_instance_config,
    std::vector<std::unique_ptr<RemoteServiceInstance>>& instances) {
    if (service_instance_config == nullptr) {
        std::cerr << "ERROR: Service instance config is nullptr!" << std::endl;
        return MakeUnexpected(mw::com::impl::ComErrc::kInvalidConfiguration);
    }
    auto ipc_instance_specifier = score::mw::com::InstanceSpecifier::Create(
                                      service_instance_config->instance_specifier()->str())
                                      .value();

    const std::string_view specifier = service_instance_config->instance_specifier()->string_view();
    IpcSkeleton ipc_skeleton = [&]() -> IpcSkeleton {
        if (specifier == "gatewayd/application_rbc_lock_status") {
            return rbc_service::CarLockUnlockStatusSkeleton::Create(ipc_instance_specifier).value();
        } else if (specifier == "gatewayd/application_rbc_hazard_lamp_status") {
            return rbc_service::HazardLampStatusSkeleton::Create(ipc_instance_specifier).value();
        } else if (specifier == "gatewayd/application_rbc_position_lamp_status") {
            return rbc_service::PositionLampStatusSkeleton::Create(ipc_instance_specifier).value();
        } else if (specifier == "gatewayd/application_rbc_approach_lamp_status") {
            return rbc_service::ApproachLampStatusSkeleton::Create(ipc_instance_specifier).value();
        }
        // TODO: Error handling
        return echo_service::EchoResponseSkeleton::Create(ipc_instance_specifier).value();
    }();

    std::cout << "Starting discovery of remote service: "
              << service_instance_config->instance_specifier()->string_view() << "\n";

    auto someipd_instance_specifier =
        score::mw::com::InstanceSpecifier::Create(std::string("gatewayd/someipd_messages")).value();

    // TODO: StartFindService should be modified to handle arbitrarily large lambdas
    // or we need to check whether it is OK to stick with dynamic allocation here.
    auto context = std::make_unique<FindServiceContext>(service_instance_config,
                                                        std::move(ipc_skeleton), instances);

    return SomeipMessageTransferProxy::StartFindService(
        [context = std::move(context)](auto handles, auto find_handle) {
            auto this_config = context->config;

            auto proxy_result = SomeipMessageTransferProxy::Create(handles.front());
            if (!proxy_result.has_value()) {
                std::cerr << "SomeipMessageTransferProxy creation failed for "
                          << this_config->instance_specifier()->string_view() << ": "
                          << proxy_result.error().Message() << "\n";
                return;
            }

            // TODO: Add mutex if callbacks can run concurrently
            context->instances.push_back(std::make_unique<RemoteServiceInstance>(
                this_config, std::move(context->skeleton), std::move(proxy_result).value()));

            std::cout << "SomeipMessageTransferProxy created for "
                      << this_config->instance_specifier()->string_view() << "\n";

            SomeipMessageTransferProxy::StopFindService(find_handle);
        },
        someipd_instance_specifier);
}

}  // namespace score::someip_gateway::gatewayd
