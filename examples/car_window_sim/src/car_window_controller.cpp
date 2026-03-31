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

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>

#include "car_window_types.h"

// mw::com runtime (your repo uses impl::Runtime::Initialize(runtime::RuntimeConfiguration))
#include "score/mw/com/impl/runtime.h"
#include "score/mw/com/runtime_configuration.h"

// Path type used by RuntimeConfiguration(filesystem::Path)
#include "score/filesystem/path.h"

namespace {

// Use the workspace-relative config path (works when running from repo root).
// If you run from bazel-bin with QEMU, ensure this file is reachable from that CWD
// OR switch to the default "./etc/mw_com_config.json" strategy.
constexpr const char* MW_COM_CONFIG_PATH = "examples/car_window_sim/config/mw_com_config.json";

constexpr const char* CONTROL_INSTANCE_SPECIFIER_ID = "carwindow/WindowControl1";

std::string ToLower(std::string s) {
    for (auto& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::string Trim(std::string s) {
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // 1) Initialize mw::com runtime with a specific config path
    // RuntimeConfiguration has:
    //   - default: "./etc/mw_com_config.json"
    //   - ctor(filesystem::Path)
    //   - ctor(argc, score::StringLiteral argv[]) with --service_instance_manifest
    score::mw::com::runtime::RuntimeConfiguration config{
        score::filesystem::Path{MW_COM_CONFIG_PATH}};

    // In your repo: Initialize returns void
    score::mw::com::impl::Runtime::Initialize(config);

    // 2) Create InstanceSpecifier from string (std::string avoids deprecated overload)
    auto instance_specifier_result =
        score::mw::com::InstanceSpecifier::Create(std::string{CONTROL_INSTANCE_SPECIFIER_ID});

    if (!instance_specifier_result.has_value()) {
        std::cerr << "Control InstanceSpecifier creation failed\n";
        return EXIT_FAILURE;
    }
    auto control_instance_specifier = instance_specifier_result.value();

    // 3) Create Skeleton and offer service
    auto skeleton_result =
        car_window_types::WindowControlSkeleton::Create(control_instance_specifier);

    if (!skeleton_result.has_value()) {
        std::cerr << "Control Skeleton creation failed\n";
        return EXIT_FAILURE;
    }

    auto skeleton = std::move(skeleton_result.value());

    auto offer_result = skeleton.OfferService();
    if (!offer_result.has_value()) {
        std::cerr << "Failed offering Control Skeleton\n";
        return EXIT_FAILURE;
    }

    std::cout << "WindowControl service offered. Type: open | close | stop | exit\n";

    // 4) Read stdin in a loop, map command, send event.
    for (;;) {
        std::string input;
        if (!std::getline(std::cin, input)) {
            std::cerr << "Error reading input (EOF or stream error)\n";
            break;
        }

        input = ToLower(Trim(input));
        if (input.empty()) {
            continue;
        }

        std::cout << "You entered: " << input << "\n";

        if (input == "exit") {
            std::cout << "Exiting...\n";
            break;
        }

        car_window_types::WindowCommand cmd{};
        if (input == "open") {
            cmd = car_window_types::WindowCommand::Open;
        } else if (input == "close") {
            cmd = car_window_types::WindowCommand::Close;
        } else if (input == "stop") {
            cmd = car_window_types::WindowCommand::Stop;
        } else {
            std::cout << "Unknown command. Please enter 'open', 'close', or 'stop'.\n";
            continue;
        }

        car_window_types::WindowControl wincontrol{};
        wincontrol.command = cmd;

        auto send_result = skeleton.window_control_.Send(wincontrol);
        if (!send_result.has_value()) {
            std::cerr << "Failed sending WindowControl event\n";
        }
    }

    // 5) Stop offering service
    skeleton.StopOfferService();
    return EXIT_SUCCESS;
}
