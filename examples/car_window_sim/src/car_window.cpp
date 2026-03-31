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

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

#include "car_window_types.h"

// mw::com runtime (your repo uses impl::Runtime::Initialize(runtime::RuntimeConfiguration))
#include "score/filesystem/path.h"
#include "score/mw/com/impl/runtime.h"
#include "score/mw/com/runtime_configuration.h"

namespace {

constexpr const char* MW_COM_CONFIG_PATH = "examples/car_window_sim/config/mw_com_config.json";
constexpr const char* CONTROL_INSTANCE_SPECIFIER_ID = "carwindow/WindowControl1";
constexpr auto SERVICE_DISCOVERY_SLEEP_DURATION = std::chrono::milliseconds(500);
constexpr auto MAIN_LOOP_SLEEP = std::chrono::milliseconds(20);

const char* to_string(car_window_types::WindowState s) {
    using car_window_types::WindowState;
    switch (s) {
        case WindowState::Open:
            return "Open";
        case WindowState::Closed:
            return "Closed";
        case WindowState::Opening:
            return "Opening";
        case WindowState::Closing:
            return "Closing";
        case WindowState::Stopped:
            return "Stopped";
    }
    return "Unknown";
}

const char* to_string(car_window_types::WindowCommand c) {
    using car_window_types::WindowCommand;
    switch (c) {
        case WindowCommand::Open:
            return "Open";
        case WindowCommand::Close:
            return "Close";
        case WindowCommand::Stop:
            return "Stop";
    }
    return "Unknown";
}

bool update_state_machine(const std::optional<car_window_types::WindowCommand>& command,
                          car_window_types::WindowInfo& winfo) {
    using car_window_types::WindowCommand;
    using car_window_types::WindowState;

    auto window_state = winfo.state;
    auto position = winfo.pos;

    if (command.has_value()) {
        switch (*command) {
            case WindowCommand::Open:
                switch (window_state) {
                    case WindowState::Open:
                        break;
                    case WindowState::Closed:
                        window_state = WindowState::Opening;
                        break;
                    case WindowState::Opening:
                        break;
                    case WindowState::Closing:
                        window_state = WindowState::Stopped;
                        break;
                    case WindowState::Stopped:
                        window_state = WindowState::Opening;
                        break;
                }
                break;

            case WindowCommand::Close:
                switch (window_state) {
                    case WindowState::Open:
                        window_state = WindowState::Closing;
                        break;
                    case WindowState::Closed:
                        break;
                    case WindowState::Opening:
                        window_state = WindowState::Stopped;
                        break;
                    case WindowState::Closing:
                        break;
                    case WindowState::Stopped:
                        window_state = WindowState::Closing;
                        break;
                }
                break;

            case WindowCommand::Stop:
                window_state = WindowState::Stopped;
                break;
        }
    } else {
        switch (window_state) {
            case WindowState::Open:
            case WindowState::Closed:
            case WindowState::Stopped:
                break;

            case WindowState::Opening:
                if (position < 100U) {
                    position += 1U;
                } else {
                    window_state = WindowState::Open;
                }
                break;

            case WindowState::Closing:
                if (position > 0U) {
                    position -= 1U;
                } else {
                    window_state = WindowState::Closed;
                }
                break;
        }
    }

    const bool changed = (position != winfo.pos) || (window_state != winfo.state);
    winfo.pos = position;
    winfo.state = window_state;
    return changed;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // 1) Initialize mw::com runtime with config file path (your repo supports Path ctor)
    score::mw::com::runtime::RuntimeConfiguration config{
        score::filesystem::Path{MW_COM_CONFIG_PATH}};
    score::mw::com::impl::Runtime::Initialize(config);

    // 2) Instance specifier (use std::string to avoid deprecated overload)
    auto instance_specifier_result =
        score::mw::com::InstanceSpecifier::Create(std::string{CONTROL_INSTANCE_SPECIFIER_ID});

    if (!instance_specifier_result.has_value()) {
        std::cerr << "Control InstanceSpecifier creation failed\n";
        return EXIT_FAILURE;
    }
    auto control_instance_specifier = instance_specifier_result.value();

    for (;;) {
        // 3) Find service handles (retry loop)
        std::vector<score::mw::com::impl::HandleType> handles;
        for (;;) {
            auto find_result =
                car_window_types::WindowControlProxy::FindService(control_instance_specifier);
            if (!find_result.has_value()) {
                std::cerr << "FindService failed, retrying...\n";
                std::this_thread::sleep_for(SERVICE_DISCOVERY_SLEEP_DURATION);
                continue;
            }

            handles = std::move(find_result).value();
            if (!handles.empty()) {
                break;
            }

            std::cout << "No control service found, retrying in 500ms\n";
            std::this_thread::sleep_for(SERVICE_DISCOVERY_SLEEP_DURATION);
        }

        // 4) Create proxy from first handle
        auto proxy_result = car_window_types::WindowControlProxy::Create(handles[0]);
        if (!proxy_result.has_value()) {
            std::cerr << "Failed to create WindowControl proxy, re-discovering...\n";
            std::this_thread::sleep_for(SERVICE_DISCOVERY_SLEEP_DURATION);
            continue;
        }
        auto proxy = std::move(proxy_result).value();

        // 5) Subscribe to event with queue depth 1 (Rust: subscribe(1))
        auto sub_result = proxy.window_control_.Subscribe(1U);
        if (!sub_result.has_value()) {
            std::cerr << "Failed to subscribe, re-discovering...\n";
            std::this_thread::sleep_for(SERVICE_DISCOVERY_SLEEP_DURATION);
            continue;
        }
        std::cout << "Subscribed!\n";

        // 6) State init
        car_window_types::WindowInfo winfo{};
        winfo.state = car_window_types::WindowState::Closed;
        winfo.pos = 0U;

        // 7) Main loop: poll samples + update state machine
        for (;;) {
            std::optional<car_window_types::WindowCommand> command = std::nullopt;

            // Get at most one new sample (equivalent to Rust get_new_sample()).
            // API note: If your repo uses a different signature (e.g., GetNewSamples callback),
            // paste the compile error and I’ll adjust.
            auto samples_result = proxy.window_control_.GetNewSamples(
                [&](score::mw::com::SamplePtr<car_window_types::WindowControl> sample) noexcept {
                    command = sample->command;
                },
                1U);

            if (!samples_result.has_value()) {
                std::cerr << "GetNewSamples failed (binding lost?). Re-discovering...\n";
                break;  // break to outer loop -> re-find service
            }

            if (command.has_value()) {
                std::cout << "Got sample: " << to_string(*command) << "\n";
            }

            if (update_state_machine(command, winfo)) {
                std::cout << "Window position: " << winfo.pos
                          << ", state: " << to_string(winfo.state) << "\n";
            }

            std::this_thread::sleep_for(MAIN_LOOP_SLEEP);
        }
    }

    return EXIT_SUCCESS;
}
