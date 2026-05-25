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

#include "local_service_instance.h"

#include <algorithm>
#include <iostream>
#include <memory>

#include "score/mw/com/types.h"
#if defined(ENABLE_KUKSA_BROKER_FEEDER)
#include "kuksa/val/v1/types.pb.h"
#include "kuksa/val/v1/val.pb.h"

#endif

using score::mw::com::GenericProxy;
using score::mw::com::SamplePtr;

namespace score::someip_gateway::gatewayd {

using network_service::interfaces::message_transfer::SomeipMessageTransferSkeleton;

static const std::size_t max_sample_count = 10;

#if defined(ENABLE_KUKSA_BROKER_FEEDER)
static const char* kParkingLightPath = "Vehicle.Body.Lights.Parking.IsOn";
static const char* kHazardLampPath = "Vehicle.Body.Lights.Hazard.IsSignaling";
static const char* kApproachLampPath = "Vehicle.Body.Trunk.Front.IsLightOn";
static const char* kDriverLockPath = "Vehicle.Cabin.Door.Row1.DriverSide.IsLocked";
static const char* kPassengerLockPath = "Vehicle.Cabin.Door.Row1.PassengerSide.IsLocked";
static std::once_flag parking_subscriber_once;
static std::atomic<bool> parking_subscriber_active{false};
static std::shared_ptr<std::thread> parking_subscriber_thread;
static std::once_flag hazard_subscriber_once;
static std::atomic<bool> hazard_subscriber_active{false};
static std::shared_ptr<std::thread> hazard_subscriber_thread;
static std::once_flag approach_subscriber_once;
static std::atomic<bool> approach_subscriber_active{false};
static std::shared_ptr<std::thread> approach_subscriber_thread;
static std::once_flag lock_unlock_subscriber_once;
static std::atomic<bool> lock_unlock_subscriber_active{false};
static std::shared_ptr<std::thread> lock_unlock_subscriber_thread;

struct BrokerSignalSubscriptionConfig {
    const char* path;
    uint16_t service_id;
    uint16_t method_id;
    const char* label;
};

static void sendBrokerSignalToSomeip(SomeipMessageTransferSkeleton& someip_message_skeleton,
                                     uint16_t service_id, uint16_t method_id, uint32_t value) {
    auto maybe_message = someip_message_skeleton.message_.Allocate();
    if (!maybe_message.has_value()) {
        std::cerr << "[gatewayd] Failed to allocate SOME/IP message" << std::endl;
        return;
    }

    auto message_sample = std::move(maybe_message).value();
    score::cpp::span<std::byte> message(
        message_sample->data, network_service::interfaces::message_transfer::MAX_MESSAGE_SIZE);

    std::size_t pos = 0;
    message.data()[pos++] = static_cast<std::byte>(service_id >> 8);
    message.data()[pos++] = static_cast<std::byte>(service_id & 0xFF);
    message.data()[pos++] = static_cast<std::byte>(method_id >> 8);
    message.data()[pos++] = static_cast<std::byte>(method_id & 0xFF);
    pos += 4;

    std::uint16_t client_id = 0xFFFF;
    message.data()[pos++] = static_cast<std::byte>(client_id >> 8);
    message.data()[pos++] = static_cast<std::byte>(client_id & 0xFF);

    std::uint16_t session_id = 0x0001;
    message.data()[pos++] = static_cast<std::byte>(session_id >> 8);
    message.data()[pos++] = static_cast<std::byte>(session_id & 0xFF);

    message.data()[pos++] = static_cast<std::byte>(0x01);
    message.data()[pos++] = static_cast<std::byte>(0x01);
    message.data()[pos++] = static_cast<std::byte>(0x01);
    message.data()[pos++] = static_cast<std::byte>(0x00);
    message.data()[pos++] = static_cast<std::byte>(value & 0xFF);
    message_sample->size = pos;
    someip_message_skeleton.message_.Send(std::move(message_sample));
}

static void subscribeToBrokerSignalStandalone(
    SomeipMessageTransferSkeleton& someip_message_skeleton,
    const BrokerSignalSubscriptionConfig& config, std::atomic<bool>& active_flag) {
    const std::string broker_addr =
        std::getenv("BROKER_ADDR") ? std::getenv("BROKER_ADDR") : "localhost:55555";
    const std::string broker_token = std::getenv("BROKER_TOKEN") ? std::getenv("BROKER_TOKEN") : "";

    std::cout << "[gatewayd] " << config.label << " subscriber connecting to databroker at "
              << broker_addr << std::endl;

    auto collector_client =
        sdv::broker_feeder::CollectorClient::createInstance(broker_addr, broker_token);
    if (!collector_client) {
        std::cerr << "[gatewayd] Failed to create collector client for " << config.label
                  << " databroker subscription" << std::endl;
        return;
    }

    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
    const bool connected = collector_client->WaitForConnected(deadline);
    std::cout << "[gatewayd] " << config.label << " subscriber WaitForConnected=" << connected
              << std::endl;
    if (!connected) {
        std::cerr << "[gatewayd] Failed to connect to databroker at " << broker_addr << " for "
                  << config.label << std::endl;
        return;
    }

    kuksa::val::v1::SubscribeRequest subscribe_request;
    auto* entry = subscribe_request.add_entries();
    entry->set_path(config.path);
    entry->set_view(kuksa::val::v1::VIEW_ALL);
    entry->add_fields(kuksa::val::v1::FIELD_VALUE);
    entry->add_fields(kuksa::val::v1::FIELD_ACTUATOR_TARGET);

    auto subscriber_context = collector_client->createClientContext();
    std::cout << "[gatewayd] Sending Subscribe request for " << config.path << std::endl;
    auto reader = collector_client->Subscribe(subscriber_context.get(), subscribe_request);

    if (!reader) {
        std::cerr << "[gatewayd] Failed to create subscriber for " << config.path << std::endl;
        return;
    }

    std::cout << "[gatewayd] Subscribed to " << config.path << " from databroker" << std::endl;

    kuksa::val::v1::SubscribeResponse response;
    while (active_flag && reader->Read(&response)) {
        std::cout << "[gatewayd] Databroker subscription message received for " << config.label
                  << " with " << response.updates().size() << " update(s)" << std::endl;
        for (const auto& update : response.updates()) {
            std::cout << "[gatewayd] Databroker update path=" << update.entry().path()
                      << " has_value=" << update.entry().has_value() << std::endl;
            if (update.entry().path() != config.path) {
                continue;
            }

            uint32_t value = 0;
            bool has_datapoint = false;
            const auto* datapoint = &update.entry().value();
            if (update.entry().has_value()) {
                has_datapoint = true;
                datapoint = &update.entry().value();
            } else if (update.entry().has_actuator_target()) {
                has_datapoint = true;
                datapoint = &update.entry().actuator_target();
                std::cout << "[gatewayd] Databroker update uses actuator_target for "
                          << config.label << std::endl;
            }

            if (!has_datapoint) {
                std::cout << "[gatewayd] Databroker update has no value/actuator_target for "
                          << config.label << std::endl;
                continue;
            }

            if (datapoint->has_uint32()) {
                value = datapoint->uint32();
                std::cout << "[gatewayd] Databroker value type=uint32 value=" << value << std::endl;
            } else if (datapoint->has_bool_()) {
                value = datapoint->bool_() ? 1 : 0;
                std::cout << "[gatewayd] Databroker value type=bool value=" << value << std::endl;
            } else {
                std::cout << "[gatewayd] Databroker value type is unsupported for " << config.label
                          << std::endl;
                continue;
            }

            std::cout << "[gatewayd] " << config.label << " update from databroker: " << value
                      << " -> Sending to SOME/IP" << std::endl;

            sendBrokerSignalToSomeip(someip_message_skeleton, config.service_id, config.method_id,
                                     value);
        }
    }

    const auto status = reader->Finish();
    std::cout << "[gatewayd] " << config.label << " subscriber finished: ok=" << status.ok()
              << " code=" << status.error_code() << " message=" << status.error_message()
              << std::endl;
}

static void subscribeToParkingLightSignalStandalone(
    SomeipMessageTransferSkeleton& someip_message_skeleton) {
    const std::string broker_addr =
        std::getenv("BROKER_ADDR") ? std::getenv("BROKER_ADDR") : "localhost:55555";
    const std::string broker_token = std::getenv("BROKER_TOKEN") ? std::getenv("BROKER_TOKEN") : "";

    std::cout << "[gatewayd] Parking light subscriber connecting to databroker at " << broker_addr
              << std::endl;

    auto collector_client =
        sdv::broker_feeder::CollectorClient::createInstance(broker_addr, broker_token);
    if (!collector_client) {
        std::cerr << "[gatewayd] Failed to create collector client for databroker subscription"
                  << std::endl;
        return;
    }

    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
    const bool connected = collector_client->WaitForConnected(deadline);
    std::cout << "[gatewayd] Parking light subscriber WaitForConnected=" << connected << std::endl;
    if (!connected) {
        std::cerr << "[gatewayd] Failed to connect to databroker at " << broker_addr << std::endl;
        return;
    }

    kuksa::val::v1::SubscribeRequest subscribe_request;
    auto* entry = subscribe_request.add_entries();
    entry->set_path(kParkingLightPath);
    entry->set_view(kuksa::val::v1::VIEW_ALL);
    entry->add_fields(kuksa::val::v1::FIELD_VALUE);
    entry->add_fields(kuksa::val::v1::FIELD_ACTUATOR_TARGET);

    auto subscriber_context = collector_client->createClientContext();
    std::cout << "[gatewayd] Sending Subscribe request for " << kParkingLightPath << std::endl;
    auto reader = collector_client->Subscribe(subscriber_context.get(), subscribe_request);

    if (!reader) {
        std::cerr << "[gatewayd] Failed to create subscriber for " << kParkingLightPath
                  << std::endl;
        return;
    }

    std::cout << "[gatewayd] Subscribed to " << kParkingLightPath << " from databroker"
              << std::endl;

    kuksa::val::v1::SubscribeResponse response;
    while (parking_subscriber_active && reader->Read(&response)) {
        std::cout << "[gatewayd] Databroker subscription message received with "
                  << response.updates().size() << " update(s)" << std::endl;
        for (const auto& update : response.updates()) {
            std::cout << "[gatewayd] Databroker update path=" << update.entry().path()
                      << " has_value=" << update.entry().has_value() << std::endl;
            if (update.entry().path() == kParkingLightPath) {
                uint32_t value = 0;
                bool has_datapoint = false;
                const auto* datapoint = &update.entry().value();
                if (update.entry().has_value()) {
                    has_datapoint = true;
                    datapoint = &update.entry().value();
                } else if (update.entry().has_actuator_target()) {
                    has_datapoint = true;
                    datapoint = &update.entry().actuator_target();
                    std::cout << "[gatewayd] Databroker update uses actuator_target" << std::endl;
                }

                if (!has_datapoint) {
                    std::cout << "[gatewayd] Databroker update has no value/actuator_target"
                              << std::endl;
                    continue;
                }

                if (datapoint->has_uint32()) {
                    value = datapoint->uint32();
                    std::cout << "[gatewayd] Databroker value type=uint32 value=" << value
                              << std::endl;
                } else if (datapoint->has_bool_()) {
                    value = datapoint->bool_() ? 1 : 0;
                    std::cout << "[gatewayd] Databroker value type=bool value=" << value
                              << std::endl;
                } else {
                    std::cout << "[gatewayd] Databroker value type is unsupported for parking light"
                              << std::endl;
                    continue;
                }

                std::cout << "[gatewayd] Parking light update from databroker: " << value
                          << " -> Sending to SOME/IP (service 0x4006)" << std::endl;

                auto maybe_message = someip_message_skeleton.message_.Allocate();
                if (!maybe_message.has_value()) {
                    std::cerr << "[gatewayd] Failed to allocate SOME/IP message" << std::endl;
                    continue;
                }

                auto message_sample = std::move(maybe_message).value();
                score::cpp::span<std::byte> message(
                    message_sample->data,
                    network_service::interfaces::message_transfer::MAX_MESSAGE_SIZE);

                std::size_t pos = 0;

                std::uint16_t service_id = 0x4006;
                message.data()[pos++] = static_cast<std::byte>(service_id >> 8);
                message.data()[pos++] = static_cast<std::byte>(service_id & 0xFF);

                std::uint16_t method_id = 0x8001;
                message.data()[pos++] = static_cast<std::byte>(method_id >> 8);
                message.data()[pos++] = static_cast<std::byte>(method_id & 0xFF);

                pos += 4;

                std::uint16_t client_id = 0xFFFF;
                message.data()[pos++] = static_cast<std::byte>(client_id >> 8);
                message.data()[pos++] = static_cast<std::byte>(client_id & 0xFF);

                std::uint16_t session_id = 0x0001;
                message.data()[pos++] = static_cast<std::byte>(session_id >> 8);
                message.data()[pos++] = static_cast<std::byte>(session_id & 0xFF);

                message.data()[pos++] = static_cast<std::byte>(0x01);
                message.data()[pos++] = static_cast<std::byte>(0x01);
                message.data()[pos++] = static_cast<std::byte>(0x01);
                message.data()[pos++] = static_cast<std::byte>(0x00);

                message.data()[pos++] = static_cast<std::byte>(value & 0xFF);
                message_sample->size = pos;
                someip_message_skeleton.message_.Send(std::move(message_sample));
            }
        }
    }

    const auto status = reader->Finish();
    std::cout << "[gatewayd] Parking light subscriber finished: ok=" << status.ok()
              << " code=" << status.error_code() << " message=" << status.error_message()
              << std::endl;
}

static void startParkingSubscriberOnce(SomeipMessageTransferSkeleton& someip_message_skeleton) {
    std::call_once(parking_subscriber_once, [&someip_message_skeleton]() {
        std::cout << "[gatewayd] Starting parking light databroker subscriber thread" << std::endl;
        parking_subscriber_active = true;
        parking_subscriber_thread = std::make_shared<std::thread>([&someip_message_skeleton]() {
            subscribeToParkingLightSignalStandalone(someip_message_skeleton);
        });
    });
}

static void startHazardSubscriberOnce(SomeipMessageTransferSkeleton& someip_message_skeleton) {
    std::call_once(hazard_subscriber_once, [&someip_message_skeleton]() {
        static const BrokerSignalSubscriptionConfig kHazardConfig = {kHazardLampPath, 0x4004,
                                                                     0x8001, "Hazard lamp"};
        std::cout << "[gatewayd] Starting hazard lamp databroker subscriber thread" << std::endl;
        hazard_subscriber_active = true;
        hazard_subscriber_thread = std::make_shared<std::thread>([&someip_message_skeleton]() {
            subscribeToBrokerSignalStandalone(someip_message_skeleton, kHazardConfig,
                                              hazard_subscriber_active);
        });
    });
}

static void startApproachSubscriberOnce(SomeipMessageTransferSkeleton& someip_message_skeleton) {
    std::call_once(approach_subscriber_once, [&someip_message_skeleton]() {
        static const BrokerSignalSubscriptionConfig kApproachConfig = {kApproachLampPath, 0x4007,
                                                                       0x8001, "Approach lamp"};
        std::cout << "[gatewayd] Starting approach lamp databroker subscriber thread" << std::endl;
        approach_subscriber_active = true;
        approach_subscriber_thread = std::make_shared<std::thread>([&someip_message_skeleton]() {
            subscribeToBrokerSignalStandalone(someip_message_skeleton, kApproachConfig,
                                              approach_subscriber_active);
        });
    });
}

static void startLockUnlockSubscriberOnce(SomeipMessageTransferSkeleton& someip_message_skeleton) {
    std::call_once(lock_unlock_subscriber_once, [&someip_message_skeleton]() {
        std::cout << "[gatewayd] Starting car lock/unlock databroker subscriber thread for "
                  << kDriverLockPath << " and " << kPassengerLockPath << std::endl;
        lock_unlock_subscriber_active = true;
        lock_unlock_subscriber_thread = std::make_shared<std::thread>([&someip_message_skeleton]() {
            const std::string broker_addr =
                std::getenv("BROKER_ADDR") ? std::getenv("BROKER_ADDR") : "localhost:55555";
            const std::string broker_token =
                std::getenv("BROKER_TOKEN") ? std::getenv("BROKER_TOKEN") : "";

            std::cout << "[gatewayd] Car lock/unlock subscriber connecting to databroker at "
                      << broker_addr << std::endl;

            auto collector_client =
                sdv::broker_feeder::CollectorClient::createInstance(broker_addr, broker_token);
            if (!collector_client) {
                std::cerr << "[gatewayd] Failed to create collector client for car lock/unlock"
                          << " databroker subscription" << std::endl;
                return;
            }

            auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
            const bool connected = collector_client->WaitForConnected(deadline);
            std::cout << "[gatewayd] Car lock/unlock subscriber WaitForConnected=" << connected
                      << std::endl;
            if (!connected) {
                std::cerr << "[gatewayd] Failed to connect to databroker at " << broker_addr
                          << " for car lock/unlock" << std::endl;
                return;
            }

            kuksa::val::v1::SubscribeRequest subscribe_request;
            auto* driver_entry = subscribe_request.add_entries();
            driver_entry->set_path(kDriverLockPath);
            driver_entry->set_view(kuksa::val::v1::VIEW_ALL);
            driver_entry->add_fields(kuksa::val::v1::FIELD_VALUE);
            driver_entry->add_fields(kuksa::val::v1::FIELD_ACTUATOR_TARGET);
            auto* passenger_entry = subscribe_request.add_entries();
            passenger_entry->set_path(kPassengerLockPath);
            passenger_entry->set_view(kuksa::val::v1::VIEW_ALL);
            passenger_entry->add_fields(kuksa::val::v1::FIELD_VALUE);
            passenger_entry->add_fields(kuksa::val::v1::FIELD_ACTUATOR_TARGET);

            auto subscriber_context = collector_client->createClientContext();
            std::cout << "[gatewayd] Sending Subscribe request for " << kDriverLockPath << " and "
                      << kPassengerLockPath << std::endl;
            auto reader = collector_client->Subscribe(subscriber_context.get(), subscribe_request);

            if (!reader) {
                std::cerr << "[gatewayd] Failed to create subscriber for car lock/unlock paths"
                          << std::endl;
                return;
            }

            std::cout << "[gatewayd] Subscribed to " << kDriverLockPath << " and "
                      << kPassengerLockPath << " from databroker" << std::endl;

            kuksa::val::v1::SubscribeResponse response;
            while (lock_unlock_subscriber_active && reader->Read(&response)) {
                for (const auto& update : response.updates()) {
                    uint16_t service_id = 0;
                    const char* label = nullptr;
                    if (update.entry().path() == kDriverLockPath) {
                        service_id = 0x4003;
                        label = "Driver-side lock";
                    } else if (update.entry().path() == kPassengerLockPath) {
                        service_id = 0x4008;
                        label = "Passenger-side lock";
                    } else {
                        continue;
                    }

                    uint32_t value = 0;
                    bool has_datapoint = false;
                    const auto* datapoint = &update.entry().value();
                    if (update.entry().has_value()) {
                        has_datapoint = true;
                        datapoint = &update.entry().value();
                    } else if (update.entry().has_actuator_target()) {
                        has_datapoint = true;
                        datapoint = &update.entry().actuator_target();
                    }

                    if (!has_datapoint) {
                        continue;
                    }

                    if (datapoint->has_uint32()) {
                        value = datapoint->uint32();
                    } else if (datapoint->has_bool_()) {
                        value = datapoint->bool_() ? 1 : 0;
                    } else {
                        continue;
                    }

                    std::cout << "[gatewayd] " << label << " update from databroker: " << value
                              << " -> Sending to SOME/IP [svc=0x" << std::hex << service_id
                              << " evt=0x8001]" << std::dec << std::endl;
                    sendBrokerSignalToSomeip(someip_message_skeleton, service_id, 0x8001, value);
                }
            }

            const auto status = reader->Finish();
            std::cout << "[gatewayd] Car lock/unlock subscriber finished: ok=" << status.ok()
                      << " code=" << status.error_code() << " message=" << status.error_message()
                      << std::endl;
        });
    });
}
#endif

LocalServiceInstance::LocalServiceInstance(
    std::shared_ptr<const config::ServiceInstance> service_instance_config,
    GenericProxy&& ipc_proxy,
    // TODO: Decouple this via an interface
    SomeipMessageTransferSkeleton& someip_message_skeleton)
    : service_instance_config_(std::move(service_instance_config)),
      ipc_proxy_(std::move(ipc_proxy)),
      someip_message_skeleton_(someip_message_skeleton) {
    // Set up IPC event handlers
    auto& events = ipc_proxy_.GetEvents();

    for (auto event_config : *service_instance_config_->events()) {
        auto result = events.find(event_config->event_name()->string_view());
        if (result == events.cend()) {
            std::cerr << "Failed to find " << event_config->event_name()->string_view()
                      << " event in ipc_proxy." << std::endl;
            continue;
        }
        auto& ipc_event = result->second;

        ipc_event.SetReceiveHandler([this, &ipc_event, event_config]() {
            ipc_event.GetNewSamples(
                [&](SamplePtr<void> sample) {
                    auto maybe_message = someip_message_skeleton_.message_.Allocate();
                    if (!maybe_message.has_value()) {
                        std::cerr << "Failed to allocate SOME/IP message:"
                                  << maybe_message.error().Message() << std::endl;
                        return;
                    }
                    auto message_sample = std::move(maybe_message).value();
                    score::cpp::span<std::byte> message(
                        message_sample->data,
                        network_service::interfaces::message_transfer::MAX_MESSAGE_SIZE);
                    std::size_t pos = 0;

                    // TODO: Design decision: the gateway needs to generate the SOME/IP message
                    // including the header in order to have the E2E protection in the ASIL
                    // context.
                    std::uint16_t service_id = service_instance_config_->someip_service_id();
                    message.data()[pos++] = static_cast<std::byte>(service_id >> 8);
                    message.data()[pos++] = static_cast<std::byte>(service_id & 0xFF);

                    std::uint16_t method_id = event_config->someip_method_id();
                    message.data()[pos++] = static_cast<std::byte>(method_id >> 8);
                    message.data()[pos++] = static_cast<std::byte>(method_id & 0xFF);

                    // Length set by someipd
                    pos += 4;

                    // TODO: get client ID during registration at the someipd
                    std::uint16_t client_id = 0xFFFF;
                    message.data()[pos++] = static_cast<std::byte>(client_id >> 8);
                    message.data()[pos++] = static_cast<std::byte>(client_id & 0xFF);

                    std::uint16_t session_id = 0x0000;
                    message.data()[pos++] = static_cast<std::byte>(session_id >> 8);
                    message.data()[pos++] = static_cast<std::byte>(session_id & 0xFF);

                    std::uint8_t protocol_version = 1;
                    message.data()[pos++] = static_cast<std::byte>(protocol_version);

                    std::uint8_t interface_version =
                        service_instance_config_->someip_service_version_major();
                    message.data()[pos++] = static_cast<std::byte>(interface_version);

                    std::uint8_t message_type = 0x02;  // NOTIFICATION
                    message.data()[pos++] = static_cast<std::byte>(message_type);

                    std::uint8_t return_code = 0x00;  // Unused
                    message.data()[pos++] = static_cast<std::byte>(return_code);

                    // Serialize payload
                    // TODO: Call serialization plugin here
                    auto payload = message.subspan(pos);
                    std::size_t payload_size = std::min(payload.size(), ipc_event.GetSampleSize());
                    std::memcpy(payload.data(), sample.get(), payload_size);
                    pos += payload_size;

                    message_sample->size = pos;

                    someip_message_skeleton_.message_.Send(std::move(message_sample));
                },
                max_sample_count);
        });

        ipc_event.Subscribe(max_sample_count);
    }
}

#if defined(ENABLE_KUKSA_BROKER_FEEDER)
void LocalServiceInstance::startBrokerSubscriber() {
    std::cout << "[gatewayd] Starting parking light databroker subscriber thread" << std::endl;
    subscriber_active_ = true;
    broker_subscriber_thread_ =
        std::make_shared<std::thread>([this]() { subscribeToParkingLightSignal(); });
}

void LocalServiceInstance::subscribeToParkingLightSignal() {
    const std::string broker_addr =
        std::getenv("BROKER_ADDR") ? std::getenv("BROKER_ADDR") : "localhost:55555";
    const std::string broker_token = std::getenv("BROKER_TOKEN") ? std::getenv("BROKER_TOKEN") : "";

    std::cout << "[gatewayd] Parking light subscriber connecting to databroker at " << broker_addr
              << std::endl;

    auto collector_client =
        sdv::broker_feeder::CollectorClient::createInstance(broker_addr, broker_token);
    if (!collector_client) {
        std::cerr << "[gatewayd] Failed to create collector client for databroker subscription"
                  << std::endl;
        return;
    }

    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
    const bool connected = collector_client->WaitForConnected(deadline);
    std::cout << "[gatewayd] Parking light subscriber WaitForConnected=" << connected << std::endl;
    if (!connected) {
        std::cerr << "[gatewayd] Failed to connect to databroker at " << broker_addr << std::endl;
        return;
    }

    // Subscribe to parking light signal
    kuksa::val::v1::SubscribeRequest subscribe_request;
    auto* entry = subscribe_request.add_entries();
    entry->set_path("Vehicle.Body.Lights.Parking.IsOn");
    entry->set_view(kuksa::val::v1::VIEW_ALL);
    entry->add_fields(kuksa::val::v1::FIELD_VALUE);
    entry->add_fields(kuksa::val::v1::FIELD_ACTUATOR_TARGET);

    auto subscriber_context = collector_client->createClientContext();
    std::cout << "[gatewayd] Sending Subscribe request for Vehicle.Body.Lights.Parking.IsOn"
              << std::endl;
    auto reader = collector_client->Subscribe(subscriber_context.get(), subscribe_request);

    if (!reader) {
        std::cerr << "[gatewayd] Failed to create subscriber for Vehicle.Body.Lights.Parking.IsOn"
                  << std::endl;
        return;
    }

    std::cout << "[gatewayd] Subscribed to Vehicle.Body.Lights.Parking.IsOn from databroker"
              << std::endl;

    kuksa::val::v1::SubscribeResponse response;
    while (subscriber_active_ && reader->Read(&response)) {
        std::cout << "[gatewayd] Databroker subscription message received with "
                  << response.updates().size() << " update(s)" << std::endl;
        for (const auto& update : response.updates()) {
            std::cout << "[gatewayd] Databroker update path=" << update.entry().path()
                      << " has_value=" << update.entry().has_value() << std::endl;
            if (update.entry().path() == "Vehicle.Body.Lights.Parking.IsOn") {
                // Extract boolean/uint32 value from value or actuator_target
                uint32_t value = 0;
                bool has_datapoint = false;
                const auto* datapoint = &update.entry().value();
                if (update.entry().has_value()) {
                    has_datapoint = true;
                    datapoint = &update.entry().value();
                } else if (update.entry().has_actuator_target()) {
                    has_datapoint = true;
                    datapoint = &update.entry().actuator_target();
                    std::cout << "[gatewayd] Databroker update uses actuator_target" << std::endl;
                }

                if (!has_datapoint) {
                    std::cout << "[gatewayd] Databroker update has no value/actuator_target"
                              << std::endl;
                    continue;
                }

                if (datapoint->has_uint32()) {
                    value = datapoint->uint32();
                    std::cout << "[gatewayd] Databroker value type=uint32 value=" << value
                              << std::endl;
                } else if (datapoint->has_bool_()) {
                    value = datapoint->bool_() ? 1 : 0;
                    std::cout << "[gatewayd] Databroker value type=bool value=" << value
                              << std::endl;
                } else {
                    std::cout << "[gatewayd] Databroker value type is unsupported for parking light"
                              << std::endl;
                    continue;
                }

                std::cout << "[gatewayd] Parking light update from databroker: " << value
                          << " → Sending to SOME/IP (service 0x4006)" << std::endl;

                // Send SOME/IP command to turn on/off parking lamp
                auto maybe_message = someip_message_skeleton_.message_.Allocate();
                if (!maybe_message.has_value()) {
                    std::cerr << "[gatewayd] Failed to allocate SOME/IP message" << std::endl;
                    continue;
                }

                auto message_sample = std::move(maybe_message).value();
                score::cpp::span<std::byte> message(
                    message_sample->data,
                    network_service::interfaces::message_transfer::MAX_MESSAGE_SIZE);

                std::size_t pos = 0;

                // SOME/IP Header: Service 0x4006 (Position Lamp Command)
                std::uint16_t service_id = 0x4006;
                message.data()[pos++] = static_cast<std::byte>(service_id >> 8);
                message.data()[pos++] = static_cast<std::byte>(service_id & 0xFF);

                // Method ID
                std::uint16_t method_id = 0x8001;
                message.data()[pos++] = static_cast<std::byte>(method_id >> 8);
                message.data()[pos++] = static_cast<std::byte>(method_id & 0xFF);

                // Length placeholder
                pos += 4;

                // Client ID, Session ID
                std::uint16_t client_id = 0xFFFF;
                message.data()[pos++] = static_cast<std::byte>(client_id >> 8);
                message.data()[pos++] = static_cast<std::byte>(client_id & 0xFF);

                std::uint16_t session_id = 0x0001;
                message.data()[pos++] = static_cast<std::byte>(session_id >> 8);
                message.data()[pos++] = static_cast<std::byte>(session_id & 0xFF);

                // Protocol/Interface version
                message.data()[pos++] = static_cast<std::byte>(0x01);  // Protocol version
                message.data()[pos++] = static_cast<std::byte>(0x01);  // Interface version
                message.data()[pos++] = static_cast<std::byte>(0x01);  // Message type (REQUEST)
                message.data()[pos++] = static_cast<std::byte>(0x00);  // Return code

                // Payload: 1-byte position lamp status
                message.data()[pos++] = static_cast<std::byte>(value & 0xFF);
                pos += 1;

                message_sample->size = pos;
                someip_message_skeleton_.message_.Send(std::move(message_sample));
            }
        }
    }

    const auto status = reader->Finish();
    std::cout << "[gatewayd] Parking light subscriber finished: ok=" << status.ok()
              << " code=" << status.error_code() << " message=" << status.error_message()
              << std::endl;

    subscriber_active_ = false;
    std::cout << "[gatewayd] Databroker subscriber stopped" << std::endl;
}
#endif

}  // namespace score::someip_gateway::gatewayd

namespace score::someip_gateway::gatewayd {
namespace {
struct FindServiceContext {
    std::shared_ptr<const config::ServiceInstance> config;
    SomeipMessageTransferSkeleton& skeleton;
    std::vector<std::unique_ptr<LocalServiceInstance>>& instances;

    FindServiceContext(std::shared_ptr<const config::ServiceInstance> config_,
                       SomeipMessageTransferSkeleton& skeleton_,
                       std::vector<std::unique_ptr<LocalServiceInstance>>& instances_)
        : config(std::move(config_)), skeleton(skeleton_), instances(instances_) {}
};

}  // namespace

Result<mw::com::FindServiceHandle> LocalServiceInstance::CreateAsyncLocalService(
    std::shared_ptr<const config::ServiceInstance> service_instance_config,
    SomeipMessageTransferSkeleton& someip_message_skeleton,
    std::vector<std::unique_ptr<LocalServiceInstance>>& instances) {
    if (service_instance_config == nullptr) {
        std::cerr << "ERROR: Service instance config is nullptr!" << std::endl;
        return MakeUnexpected(mw::com::impl::ComErrc::kInvalidConfiguration);
    }
    auto instance_specifier_result = score::mw::com::InstanceSpecifier::Create(
        service_instance_config->instance_specifier()->str());
    if (!instance_specifier_result.has_value()) {
        std::cerr << "ERROR: Failed to resolve local instance specifier '"
                  << service_instance_config->instance_specifier()->string_view()
                  << "'. The deployed mw_com_config.json likely does not contain this entry."
                  << std::endl;
        return MakeUnexpected(mw::com::impl::ComErrc::kInvalidConfiguration);
    }
    auto instance_specifier = std::move(instance_specifier_result).value();

#if defined(ENABLE_KUKSA_BROKER_FEEDER)
    if (service_instance_config->instance_specifier()->string_view() ==
        "gatewayd/application_rbc_position_lamp_cmd") {
        std::cout << "[gatewayd] Enabling databroker subscriber for "
                  << service_instance_config->instance_specifier()->string_view() << std::endl;
        startParkingSubscriberOnce(someip_message_skeleton);
    } else if (service_instance_config->instance_specifier()->string_view() ==
               "gatewayd/application_rbc_lock_unlock_cmd") {
        std::cout << "[gatewayd] Enabling databroker subscriber for "
                  << service_instance_config->instance_specifier()->string_view() << std::endl;
        startLockUnlockSubscriberOnce(someip_message_skeleton);
    } else if (service_instance_config->instance_specifier()->string_view() ==
               "gatewayd/application_rbc_hazard_lamp_cmd") {
        std::cout << "[gatewayd] Enabling databroker subscriber for "
                  << service_instance_config->instance_specifier()->string_view() << std::endl;
        startHazardSubscriberOnce(someip_message_skeleton);
    } else if (service_instance_config->instance_specifier()->string_view() ==
               "gatewayd/application_rbc_approach_lamp_cmd") {
        std::cout << "[gatewayd] Enabling databroker subscriber for "
                  << service_instance_config->instance_specifier()->string_view() << std::endl;
        startApproachSubscriberOnce(someip_message_skeleton);
    }
#endif

    std::cout << "Starting discovery: "
              << service_instance_config->instance_specifier()->string_view() << "\n";

    // TODO: StartFindService should be modified to handle arbitrarily large lambdas
    // or we need to check whether it is OK to stick with dynamic allocation here.
    auto context = std::make_unique<FindServiceContext>(service_instance_config,
                                                        someip_message_skeleton, instances);

    return GenericProxy::StartFindService(
        [context = std::move(context)](auto handles, auto find_handle) {
            auto& this_config = context->config;

            auto proxy_result = GenericProxy::Create(handles.front());
            if (!proxy_result.has_value()) {
                std::cerr << "Proxy creation failed: "
                          << this_config->instance_specifier()->string_view() << "\n";
                return;
            }

            // TODO: Add mutex if callbacks can run concurrently or use futures
            context->instances.push_back(std::make_unique<LocalServiceInstance>(
                this_config, std::move(proxy_result).value(), context->skeleton));

            std::cout << "Proxy created: " << this_config->instance_specifier()->string_view()
                      << "\n";

            GenericProxy::StopFindService(find_handle);
        },
        instance_specifier);
}

}  // namespace score::someip_gateway::gatewayd
