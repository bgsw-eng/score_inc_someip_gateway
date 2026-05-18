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

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

#include "local_service_instance.h"
#include "remote_service_instance.h"
#include "score/mw/com/runtime.h"
#include "score/mw/com/types.h"
#include "src/gatewayd/gatewayd_config_generated.h"
#include "src/network_service/interfaces/message_transfer.h"

// In the main file we are not in any namespace
using namespace score::someip_gateway::gatewayd;
using score::someip_gateway::network_service::interfaces::message_transfer::
    SomeipMessageTransferSkeleton;

// Global flag to control application shutdown
static std::atomic<bool> shutdown_requested{false};

// Signal handler for graceful shutdown
void termination_handler(int /*signal*/) {
    std::cout << "Received termination signal. Initiating graceful shutdown..." << std::endl;
    shutdown_requested.store(true);
}

bool SendLampCommand(SomeipMessageTransferSkeleton& someip_message_skeleton,
                     std::uint16_t service_id, std::uint16_t method_id, std::uint8_t state,
                     const char* lamp_name) {
    auto maybe_message = someip_message_skeleton.message_.Allocate();
    if (!maybe_message.has_value()) {
        std::cerr << "[gatewayd] ERROR: Failed to allocate SOME/IP message for " << lamp_name
                  << ": " << maybe_message.error().Message() << std::endl;
        return false;
    }

    auto message_sample = std::move(maybe_message).value();
    std::byte* message_data = message_sample->data;
    std::size_t pos = 0;

    message_data[pos++] = static_cast<std::byte>(service_id >> 8);
    message_data[pos++] = static_cast<std::byte>(service_id & 0xFF);

    message_data[pos++] = static_cast<std::byte>(method_id >> 8);
    message_data[pos++] = static_cast<std::byte>(method_id & 0xFF);

    // Length is filled by someipd, keep 4-byte placeholder.
    pos += 4;

    const std::uint16_t client_id = 0xFFFF;
    message_data[pos++] = static_cast<std::byte>(client_id >> 8);
    message_data[pos++] = static_cast<std::byte>(client_id & 0xFF);

    const std::uint16_t session_id = 0x0000;
    message_data[pos++] = static_cast<std::byte>(session_id >> 8);
    message_data[pos++] = static_cast<std::byte>(session_id & 0xFF);

    const std::uint8_t protocol_version = 1;
    message_data[pos++] = static_cast<std::byte>(protocol_version);

    const std::uint8_t interface_version = 0x01;
    message_data[pos++] = static_cast<std::byte>(interface_version);

    const std::uint8_t message_type = 0x02;  // NOTIFICATION/EVENT
    message_data[pos++] = static_cast<std::byte>(message_type);

    const std::uint8_t return_code = 0x00;
    message_data[pos++] = static_cast<std::byte>(return_code);

    message_data[pos++] = static_cast<std::byte>(state);
    message_sample->size = pos;

    someip_message_skeleton.message_.Send(std::move(message_sample));

    std::cout << "[gatewayd] ✓ " << lamp_name << " command sent"
              << " (service=0x" << std::hex << service_id << ", method=0x" << method_id
              << ", payload=0x" << static_cast<int>(state) << std::dec << ")" << std::endl;
    return true;
}

int main(int argc, const char* argv[]) {
    // Register signal handlers for graceful shutdown
    std::signal(SIGTERM, termination_handler);
    std::signal(SIGINT, termination_handler);

    // Read config data
    // TODO: Be more flexible with the path
    // TODO: Use memory mapped file instead of copying into buffer
    std::ifstream config_file;
    config_file.open("src/gatewayd/etc/gatewayd_config.bin", std::ios::binary | std::ios::in);

    if (!config_file.is_open()) {
        std::cerr << "Error: Could not open config file 'src/gatewayd/etc/gatewayd_config.bin'"
                  << std::endl;
        return 1;
    }

    config_file.seekg(0, std::ios::end);
    std::streampos length = config_file.tellg();

    if (length <= 0) {
        std::cerr << "Error: Invalid config file size: " << length << std::endl;
        config_file.close();
        return 1;
    }

    config_file.seekg(0, std::ios::beg);
    auto config_buffer = std::shared_ptr<char>(new char[length]);
    config_file.read(config_buffer.get(), length);
    config_file.close();

    auto config =
        std::shared_ptr<const config::Root>(config_buffer, config::GetRoot(config_buffer.get()));

    score::mw::com::runtime::InitializeRuntime(argc, argv);

    // TODO: Need to come up with a proper scheme how to generate instance specifiers
    auto create_result = SomeipMessageTransferSkeleton::Create(
        score::mw::com::InstanceSpecifier::Create(std::string("gatewayd/gatewayd_messages"))
            .value());
    // TODO: Error handling
    auto someip_message_skeleton = std::move(create_result).value();

    // TODO: Error handling
    (void)someip_message_skeleton.OfferService();

    // Create service instances from configuration
    if (config->local_service_instances() == nullptr) {
        std::cerr << "No local service instances configured" << std::endl;
        return 1;
    }

    std::vector<std::unique_ptr<LocalServiceInstance>> local_service_instances;
    for (auto service_instance_config : *config->local_service_instances()) {
        const auto instance_name = service_instance_config->instance_specifier()->string_view();
        std::cout << "[gatewayd] Local discovery init: " << instance_name << std::endl;
        auto local_find_result = LocalServiceInstance::CreateAsyncLocalService(
            std::shared_ptr<const config::ServiceInstance>(config, service_instance_config),
            someip_message_skeleton, local_service_instances);
        if (!local_find_result.has_value()) {
            std::cerr << "[gatewayd] Local discovery start failed for " << instance_name
                      << std::endl;
        } else {
            std::cout << "[gatewayd] Local discovery started for " << instance_name << std::endl;
        }
    }

    // Create service instances from configuration
    if (config->remote_service_instances() == nullptr) {
        std::cerr << "No remote service instances configured" << std::endl;
        return 1;
    }

    std::vector<std::unique_ptr<RemoteServiceInstance>> remote_service_instances;
    for (auto service_instance_config : *config->remote_service_instances()) {
        const auto instance_name = service_instance_config->instance_specifier()->string_view();
        std::cout << "[gatewayd] Remote discovery init: " << instance_name << std::endl;
        auto remote_find_result = RemoteServiceInstance::CreateAsyncRemoteService(
            std::shared_ptr<const config::ServiceInstance>(config, service_instance_config),
            remote_service_instances);
        if (!remote_find_result.has_value()) {
            std::cerr << "[gatewayd] Remote discovery start failed for " << instance_name
                      << std::endl;
        } else {
            std::cout << "[gatewayd] Remote discovery started for " << instance_name << std::endl;
        }
    }

    std::cout << "Gateway started, waiting for shutdown signal..." << std::endl;

    // Main loop - command flow is event-driven via IPC and databroker subscriptions.
    while (!shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "Shutting down gateway..." << std::endl;

    return 0;
}
