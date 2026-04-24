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

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>

#include "score/mw/com/types.h"

#if defined(ENABLE_KUKSA_BROKER_FEEDER)
#include "collector_client.h"
#include "create_datapoint.h"
#include "data_broker_feeder.h"
#endif

using score::mw::com::GenericProxy;
using score::mw::com::SamplePtr;

namespace score::someip_gateway::gatewayd {

using network_service::interfaces::message_transfer::SomeipMessageTransferProxy;

static const std::size_t max_sample_count = 10;
static const std::size_t SOMEIP_FULL_HEADER_SIZE = 16;

static const char* kSomeipLastServiceIdPath = "Vehicle.Private.Gatewayd.Someip.LastServiceId";
static const char* kSomeipLastEventIdPath = "Vehicle.Private.Gatewayd.Someip.LastEventId";
static const char* kSomeipLastPayloadBytePath = "Vehicle.Private.Gatewayd.Someip.LastPayloadByte";

// RBC signal mappings (received from SOME/IP, forwarded directly to KUKSA databroker)
// LOCK STATUS   (0x3003/0x8002) → Vehicle.Powertrain.ElectricMotor.MU1_Reserved_01
// HAZARD LAMP   (0x3003/0x8003) → Vehicle.Powertrain.ElectricMotor.MU1_Reserved_02
// POSITION LAMP (0x3003/0x8004) → Vehicle.Powertrain.ElectricMotor.MU1_Reserved_03
// APPROACH LAMP (0x3004/0x8009) → Vehicle.Powertrain.TractionBattery.DTE.MU2_Reserved01

#if defined(ENABLE_KUKSA_BROKER_FEEDER)
static const char* kBrokerAddrEnv = "BROKER_ADDR";
static const char* kBrokerTokenEnv = "BROKER_TOKEN";
static const char* kDefaultBrokerAddr = "localhost:55555";

static std::mutex broker_feeder_mutex;
static std::shared_ptr<sdv::broker_feeder::CollectorClient> collector_client;
static std::shared_ptr<sdv::broker_feeder::DataBrokerFeeder> broker_feeder;
static std::shared_ptr<std::thread> broker_feeder_thread;
static std::atomic<bool> broker_feeder_active{false};

static std::string getEnvOrDefault(const char* name, const std::string& fallback) {
    auto value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

static void initBrokerFeeder() {
    if (broker_feeder_active) {
        return;  // Already initialized, fast path without lock
    }
    std::lock_guard<std::mutex> lock(broker_feeder_mutex);
    if (broker_feeder_active) {
        return;  // Double-checked after acquiring lock
    }
    const std::string broker_addr = getEnvOrDefault(kBrokerAddrEnv, kDefaultBrokerAddr);
    const std::string broker_token = getEnvOrDefault(kBrokerTokenEnv, std::string{});
    std::cout << "[gatewayd] Attempting to initialize Kuksa broker feeder at " << broker_addr
              << std::endl;

    sdv::broker_feeder::DatapointConfiguration metadata = {
        {"Vehicle.Powertrain.ElectricMotor.MU1_Reserved_01", sdv::databroker::v1::DataType::UINT32,
         sdv::databroker::v1::ChangeType::ON_CHANGE, sdv::broker_feeder::createNotAvailableValue(),
         "RBC lock status from SOME/IP 0x3003/0x8002"},
        {"Vehicle.Powertrain.ElectricMotor.MU1_Reserved_02", sdv::databroker::v1::DataType::UINT32,
         sdv::databroker::v1::ChangeType::ON_CHANGE, sdv::broker_feeder::createNotAvailableValue(),
         "RBC hazard lamp from SOME/IP 0x3003/0x8003"},
        {"Vehicle.Powertrain.ElectricMotor.MU1_Reserved_03", sdv::databroker::v1::DataType::UINT32,
         sdv::databroker::v1::ChangeType::ON_CHANGE, sdv::broker_feeder::createNotAvailableValue(),
         "RBC position lamp from SOME/IP 0x3003/0x8004"},
        {"Vehicle.Powertrain.TractionBattery.DTE.MU2_Reserved01",
         sdv::databroker::v1::DataType::UINT32, sdv::databroker::v1::ChangeType::ON_CHANGE,
         sdv::broker_feeder::createNotAvailableValue(),
         "RBC approach lamp from SOME/IP 0x3004/0x8009"},
        {kSomeipLastServiceIdPath, sdv::databroker::v1::DataType::UINT32,
         sdv::databroker::v1::ChangeType::ON_CHANGE, sdv::broker_feeder::createNotAvailableValue(),
         "Last SOME/IP service id received by gatewayd"},
        {kSomeipLastEventIdPath, sdv::databroker::v1::DataType::UINT32,
         sdv::databroker::v1::ChangeType::ON_CHANGE, sdv::broker_feeder::createNotAvailableValue(),
         "Last SOME/IP event id received by gatewayd"},
        {kSomeipLastPayloadBytePath, sdv::databroker::v1::DataType::UINT32,
         sdv::databroker::v1::ChangeType::ON_CHANGE, sdv::broker_feeder::createNotAvailableValue(),
         "Last SOME/IP payload first byte received by gatewayd"},
    };

    collector_client =
        sdv::broker_feeder::CollectorClient::createInstance(broker_addr, broker_token);
    broker_feeder =
        sdv::broker_feeder::DataBrokerFeeder::createInstance(collector_client, std::move(metadata));

    if (!broker_feeder) {
        std::cerr << "[gatewayd] DataBrokerFeeder::createInstance returned null, will retry."
                  << std::endl;
        collector_client.reset();
        return;
    }
    std::cout << "[gatewayd] DataBrokerFeeder instance created, starting feeder thread..."
              << std::endl;

    broker_feeder_thread =
        std::make_shared<std::thread>(&sdv::broker_feeder::DataBrokerFeeder::Run, broker_feeder);
    broker_feeder_active = true;
    std::cout << "[gatewayd] Kuksa broker feeder initialized for " << broker_addr << std::endl;
}

static void shutdownBrokerFeeder() {
    if (broker_feeder_active && broker_feeder) {
        broker_feeder->Shutdown();
        broker_feeder_active = false;
    }
    if (broker_feeder_thread && broker_feeder_thread->joinable()) {
        broker_feeder_thread->join();
    }
}

struct BrokerFeederShutdownGuard {
    ~BrokerFeederShutdownGuard() { shutdownBrokerFeeder(); }
};

static BrokerFeederShutdownGuard broker_feeder_shutdown_guard;
#endif

static void writeSomeipToDatabroker(uint16_t svc_id, uint16_t evt_id,
                                    const score::cpp::span<const std::byte>& payload,
                                    const char* signal_name) {
    // Extract value from SOME/IP payload (typically 1 byte)
    if (payload.empty()) {
        return;
    }
    uint32_t value = static_cast<uint32_t>(static_cast<uint8_t>(payload.data()[0]));
    std::string rbc_vss_path;

    // Map SOME/IP service:event to signal key and VSS path
    if (svc_id == 0x3003 && evt_id == 0x8002) {
        rbc_vss_path = "Vehicle.Powertrain.ElectricMotor.MU1_Reserved_01";
    } else if (svc_id == 0x3003 && evt_id == 0x8003) {
        rbc_vss_path = "Vehicle.Powertrain.ElectricMotor.MU1_Reserved_02";
    } else if (svc_id == 0x3003 && evt_id == 0x8004) {
        rbc_vss_path = "Vehicle.Powertrain.ElectricMotor.MU1_Reserved_03";
    } else if (svc_id == 0x3004 && evt_id == 0x8009) {
        rbc_vss_path = "Vehicle.Powertrain.TractionBattery.DTE.MU2_Reserved01";
    }

#if defined(ENABLE_KUKSA_BROKER_FEEDER)
    initBrokerFeeder();
    if (broker_feeder_active && broker_feeder) {
        broker_feeder->FeedValue(kSomeipLastServiceIdPath,
                                 sdv::broker_feeder::createDatapoint(static_cast<uint32_t>(svc_id)));
        broker_feeder->FeedValue(kSomeipLastEventIdPath,
                                 sdv::broker_feeder::createDatapoint(static_cast<uint32_t>(evt_id)));
        broker_feeder->FeedValue(kSomeipLastPayloadBytePath,
                                 sdv::broker_feeder::createDatapoint(value));

        if (!rbc_vss_path.empty()) {
            std::cout << "[gatewayd] " << signal_name << " (" << rbc_vss_path << ") = " << value
                      << " → KUKSA Databroker" << std::endl;
            broker_feeder->FeedValue(rbc_vss_path, sdv::broker_feeder::createDatapoint(value));
        }
        return;
    }
#endif
    // Feeder not available
    std::cerr << "[gatewayd] ERROR: " << signal_name << " (svc=0x" << std::hex << svc_id
              << ", evt=0x" << evt_id << std::dec
              << ") received but feeder not initialized" << std::endl;
}

RemoteServiceInstance::RemoteServiceInstance(
    std::shared_ptr<const config::ServiceInstance> service_instance_config,
    IpcSkeleton&& ipc_skeleton, SomeipMessageTransferProxy someip_message_proxy)
    : service_instance_config_(std::move(service_instance_config)),
      ipc_skeleton_(std::move(ipc_skeleton)),
      someip_message_proxy_(std::move(someip_message_proxy)) {
#if defined(ENABLE_KUKSA_BROKER_FEEDER)
    std::cout << "[gatewayd] KUKSA broker feeder support: ENABLED" << std::endl;
#else
    std::cout << "[gatewayd] KUKSA broker feeder support: DISABLED (not compiled in)" << std::endl;
#endif
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

                        }
                    },
                    ipc_skeleton_);

                // Also write incoming SOME/IP signals directly to databroker.
                writeSomeipToDatabroker(svc_id, evt_id, payload, signal_name);
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
