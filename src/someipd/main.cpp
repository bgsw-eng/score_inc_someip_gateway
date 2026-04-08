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
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <vsomeip/defines.hpp>
#include <vsomeip/primitive_types.hpp>
#include <vsomeip/vsomeip.hpp>

// #include "score/mw/com/runtime.h"
#include "score/span.hpp"
#include "src/network_service/interfaces/message_transfer.h"

const char* someipd_name = "someipd";

static const vsomeip::service_t service_id = 0x1111;
static const vsomeip::instance_t service_instance_id = 0x2222;
static const vsomeip::method_t service_method_id = 0x3333;
static const std::size_t max_sample_count = 10;

// RBC (READ only) — signal table driven, see rbc_signals[] in main()
static const vsomeip::instance_t RBC_INSTANCE_ID = 0x0001;

// ---------------------------------------------------------------------------
// Mode selection: build with --define canoe_mode=true to use CANoe-ARC values.
// Default (no flag) keeps the existing hardcoded values.
// ---------------------------------------------------------------------------
// ---- SOME/IP service identifiers ----
#define SAMPLE_SERVICE_ID 0x1234
#define RESPONSE_SAMPLE_SERVICE_ID 0x4321
#define SAMPLE_INSTANCE_ID 0x5678
#define SAMPLE_METHOD_ID 0x0421
#define SAMPLE_EVENT_ID 0x8778
#define SAMPLE_GET_METHOD_ID 0x0001
#define SAMPLE_SET_METHOD_ID 0x0002
#define SAMPLE_EVENTGROUP_ID 0x4465
#define OTHER_SAMPLE_SERVICE_ID 0x0248
#define OTHER_SAMPLE_INSTANCE_ID 0x5422
#define OTHER_SAMPLE_METHOD_ID 0x1421

/*using score::someip_gateway::network_service::interfaces::message_transfer::
    SomeipMessageTransferProxy;
using score::someip_gateway::network_service::interfaces::message_transfer::
    SomeipMessageTransferSkeleton;
*/
// Global flag to control application shutdown
static std::atomic<bool> shutdown_requested{false};

// Guards to prevent spawning multiple subscribe threads when ON_AVAILABLE fires repeatedly
static std::atomic<bool> rbc3003_subscribed{false};
static std::atomic<bool> rbc3004_subscribed{false};

// Mutex to protect multiple client access (if needed)
static std::mutex client_mutex;

// Signal handler for graceful shutdown
void termination_handler(int /*signal*/) {
    std::cout << "Received termination signal. Initiating graceful shutdown..." << std::endl;
    shutdown_requested.store(true);
}

int main(int argc, const char* argv[]) {
    // Register signal handlers for graceful shutdown
    std::signal(SIGTERM, termination_handler);
    std::signal(SIGINT, termination_handler);

    //  score::mw::com::runtime::InitializeRuntime(argc, argv);

    auto runtime = vsomeip::runtime::get();
    auto application = runtime->create_application(someipd_name);
    if (!application->init()) {
        std::cerr << "App init failed" << std::endl;
        return 1;
    }

    std::thread([application]() {
        /*     auto handles =
                 SomeipMessageTransferProxy::FindService(
                     score::mw::com::InstanceSpecifier::Create(std::string("someipd/gatewayd_messages"))
                         .value())
                     .value();

             {  // Proxy for receiving messages from gatewayd to be sent via SOME/IP
                 auto proxy = SomeipMessageTransferProxy::Create(handles.front()).value();
                 proxy.message_.Subscribe(max_sample_count);

                 // Skeleton for transmitting messages from the network to gatewayd
                 auto create_result = SomeipMessageTransferSkeleton::Create(
                     score::mw::com::InstanceSpecifier::Create(std::string("someipd/someipd_messages"))
                         .value());
                 // TODO: Error handling
                 auto skeleton = std::move(create_result).value();
                 (void)skeleton.OfferService();

                 application->register_message_handler(
                     RESPONSE_SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID,
     */
        // -------------------------------
        // Message handler for received events
        // -------------------------------

        application->register_message_handler(
            SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID,
            //  [&skeleton](const std::shared_ptr<vsomeip::message>& msg) {
            [](const std::shared_ptr<vsomeip::message>& msg) {
                std::lock_guard<std::mutex> lock(client_mutex);
                /*    auto maybe_message = skeleton.message_.Allocate();
                    if (!maybe_message.has_value()) {
                        std::cerr << "Failed to allocate SOME/IP message:"
                                  << maybe_message.error().Message() << std::endl;
                        return;
                    }

                    auto message_sample = std::move(maybe_message).value();
                    memcpy(message_sample->data + VSOMEIP_FULL_HEADER_SIZE,
                           msg->get_payload()->get_data(), msg->get_payload()->get_length());
                    message_sample->size =
                        msg->get_payload()->get_length() + VSOMEIP_FULL_HEADER_SIZE;
                    skeleton.message_.Send(std::move(message_sample));*/
                // Internal loopback test event — suppress output
                (void)msg;
            });

        // -------------------------------
        // Re-subscribe to RBC services after every routing re-registration.
        // VSSService (routing master) can drop subscribe requests during its
        // BLOCKING CALL bursts; re-issuing on state change ensures recovery.
        // -------------------------------
        application->register_state_handler([application](vsomeip::state_type_e state) {
            if (state == vsomeip::state_type_e::ST_REGISTERED) {
                std::cout << ">>> Routing registered — re-requesting RBC services" << std::endl;
                // request_service is called once in the for loop below.
                // No duplicate calls here to avoid unbalanced ref-counts.
            }
        });

        // -------------------------------
        // Register events BEFORE availability handlers so that subscribe() calls
        // inside the handlers find already-registered events and are not dropped.
        // -------------------------------
        {
            std::set<vsomeip::eventgroup_t> eg2{0x0002};
            application->request_event(0x3003, RBC_INSTANCE_ID, 0x8002, eg2,
                                       vsomeip::event_type_e::ET_EVENT);
            std::set<vsomeip::eventgroup_t> eg3{0x0003};
            application->request_event(0x3003, RBC_INSTANCE_ID, 0x8003, eg3,
                                       vsomeip::event_type_e::ET_EVENT);
            std::set<vsomeip::eventgroup_t> eg4{0x0004};
            application->request_event(0x3003, RBC_INSTANCE_ID, 0x8004, eg4,
                                       vsomeip::event_type_e::ET_EVENT);
            std::set<vsomeip::eventgroup_t> eg9{0x0009};
            application->request_event(0x3004, RBC_INSTANCE_ID, 0x8009, eg9,
                                       vsomeip::event_type_e::ET_EVENT);
        }

        // -------------------------------
        // RBC signal table
        // -------------------------------
        struct RbcSignal {
            vsomeip::service_t service_id;
            vsomeip::event_t event_id;
            vsomeip::eventgroup_t eventgroup_id;
            const char* label;
            const char* off_label;
            const char* on_label;
        };

        static const RbcSignal rbc_signals[] = {
            {0x3003, 0x8002, 0x0002, "LOCK STATUS", "Car Unlocked", "Car Locked"},
            {0x3003, 0x8003, 0x0003, "HAZARD LAMP", "Hazard lamp OFF", "Hazard lamp ON"},
            {0x3003, 0x8004, 0x0004, "POSITION LAMP", "Position lamp OFF", "Position lamp ON"},
            {0x3004, 0x8009, 0x0009, "APPROACH LAMP", "Approach lamp OFF", "Approach lamp ON"},
        };

        // Track last received value per RBC signal (-1 = not yet received)
        static std::array<int, std::size(rbc_signals)> rbc_last_value;
        rbc_last_value.fill(-1);

        // -------------------------------
        // Step 1 — Register ALL message handlers BEFORE availability handlers.
        // This ensures no event can arrive in the window between on_available firing
        // and the for loop registering later handlers (would be silently dropped).
        // -------------------------------
        for (std::size_t sig_idx = 0; sig_idx < std::size(rbc_signals); ++sig_idx) {
            const auto& sig = rbc_signals[sig_idx];
            application->register_message_handler(
                sig.service_id, RBC_INSTANCE_ID, sig.event_id,
                [sig, sig_idx](const std::shared_ptr<vsomeip::message>& msg) {
                    std::lock_guard<std::mutex> lock(client_mutex);
                    auto data = msg->get_payload()->get_data();
                    auto len = msg->get_payload()->get_length();
                    if (len < 1) {
                        std::cout << ">>> RBC " << sig.label << ": Payload too short" << std::endl;
                        return;
                    }
                    const int v = static_cast<uint8_t>(data[0]);
                    // Only print when value has changed
                    if (v == rbc_last_value[sig_idx]) {
                        return;
                    }
                    rbc_last_value[sig_idx] = v;
                    std::cout << ">>> RBC " << sig.label << " CHANGED <<<"
                              << " [service=0x" << std::hex << sig.service_id << " event=0x"
                              << sig.event_id << std::dec << "]" << std::endl;
                    std::cout << "Raw payload (" << len << " byte(s)): ";
                    for (vsomeip::length_t i = 0; i < len; i++)
                        std::cout << std::hex << std::setw(2) << std::setfill('0')
                                  << static_cast<int>(data[i]) << " ";
                    std::cout << std::dec << std::endl;
                    std::cout << "Value: " << v << " -> " << (v == 0 ? sig.off_label : sig.on_label)
                              << std::endl;
                });
        }

        // -------------------------------
        // Step 2 — Register availability handlers. subscribe() is posted via a
        // detached thread so it does not re-enter the vsomeip dispatch thread.
        // All message handlers above are already registered at this point.
        // -------------------------------
        application->register_availability_handler(
            0x3003, RBC_INSTANCE_ID,
            [application](vsomeip::service_t svc, vsomeip::instance_t inst, bool available) {
                if (available) {
                    std::cout << ">>> RBC 0x3003 available — subscribing eg=0x2/3/4" << std::endl;
                    if (rbc3003_subscribed.exchange(true)) {
                        return;  // already subscribed, skip
                    }
                    std::thread([application, svc, inst]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        application->subscribe(svc, inst, 0x0002, 0x01);
                        application->subscribe(svc, inst, 0x0003, 0x01);
                        application->subscribe(svc, inst, 0x0004, 0x01);
                    }).detach();
                } else {
                    std::cout << ">>> RBC 0x3003 unavailable" << std::endl;
                    rbc3003_subscribed.store(false);
                }
            });

        application->register_availability_handler(
            0x3004, RBC_INSTANCE_ID,
            [application](vsomeip::service_t svc, vsomeip::instance_t inst, bool available) {
                if (available) {
                    std::cout << ">>> RBC 0x3004 available — subscribing eg=0x9" << std::endl;
                    if (rbc3004_subscribed.exchange(true)) {
                        return;  // already subscribed, skip
                    }
                    std::thread([application, svc, inst]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        application->subscribe(svc, inst, 0x0009, 0x01);
                    }).detach();
                } else {
                    std::cout << ">>> RBC 0x3004 unavailable" << std::endl;
                    rbc3004_subscribed.store(false);
                }
            });

        // -------------------------------
        // Step 3 — request_service: triggers SD + availability callback → subscribe.
        // Called once per unique service (not per signal) to avoid ref-count imbalance.
        // -------------------------------
        application->request_service(0x3003, RBC_INSTANCE_ID, 0x01);
        application->request_service(0x3004, RBC_INSTANCE_ID, 0x01);

        // -------------------------------
        // Service Discovery (SD) active
        // -------------------------------
        std::set<vsomeip::eventgroup_t> groups{SAMPLE_EVENTGROUP_ID};

        // Offer own service → SD advertises this service to network
        application->offer_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);

        // Offer an event → makes it discoverable
        application->offer_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID, groups);

        // Request own service/event → triggers SD discovery for remote services
        application->request_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
        application->request_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID, groups,
                                   vsomeip::event_type_e::ET_EVENT);

        // Subscribe to event group → uses SD to manage subscriptions
        application->subscribe(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENTGROUP_ID);

        // std::set<vsomeip::eventgroup_t> its_groups;
        // its_groups.insert(SAMPLE_EVENTGROUP_ID);

        // Prepare payload
        auto payload = vsomeip::runtime::get()->create_payload();
        size_t event_count = 0;

        //     std::cout << "SOME/IP daemon started, waiting for messages..." << std::endl;
        std::cout << "SOME/IP daemon started..." << std::endl;

        // Periodic re-subscribe counter: re-issue RBC subscribes every 2 seconds
        // to recover from VSSService routing master drops (BLOCKING CALL bursts).
        int resubscribe_tick = 0;

        while (!shutdown_requested.load()) {
            //     if (event_count < max_events) {
            // TODO: Use ReceiveHandler + async runtime instead of polling
            // static bool sent = false;

            std::vector<vsomeip::byte_t> test_data = {
                static_cast<vsomeip::byte_t>(0x11 + (event_count % 256)), 0x22, 0x33, 0x44};
            payload->set_data(test_data);

            // std::cout << "Sending test SOME/IP event #" << event_count << std::endl;

            // Notify all subscribers
            application->notify(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID,
                                payload);

            event_count++;

            // Every 4 ticks (2 seconds) re-issue RBC subscribes.
            // unsubscribe first to reset ST_NOT_ACKNOWLEDGED so vsomeip
            // re-sends the SD SubscribeEventgroup with a UDP endpoint option.
            if (++resubscribe_tick % 4 == 0) {
                application->unsubscribe(0x3003, RBC_INSTANCE_ID, 0x0002);
                application->unsubscribe(0x3003, RBC_INSTANCE_ID, 0x0003);
                application->unsubscribe(0x3003, RBC_INSTANCE_ID, 0x0004);
                application->unsubscribe(0x3004, RBC_INSTANCE_ID, 0x0009);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                application->subscribe(0x3003, RBC_INSTANCE_ID, 0x0002, 0x01);
                application->subscribe(0x3003, RBC_INSTANCE_ID, 0x0003, 0x01);
                application->subscribe(0x3003, RBC_INSTANCE_ID, 0x0004, 0x01);
                application->subscribe(0x3004, RBC_INSTANCE_ID, 0x0009, 0x01);
            }
            //     }

            /*
                    proxy.message_.GetNewSamples(
                        [&](auto message_sample) {
                std::cout << ">>> MESSAGE RECEIVED <<<" << std::endl;
                            },/*
                            // TODO: Check if size is larger than capacity of data
                            score::cpp::span<const std::byte> message(message_sample->data,
                                                                      message_sample->size);

                            // Check if sample size is valid and contains at least a SOME/IP header
                            if (message.size() < VSOMEIP_FULL_HEADER_SIZE) {
                                std::cerr << "Received too small sample (size: " << message.size()
                                          << ", expected at least: " << VSOMEIP_FULL_HEADER_SIZE
                                          << "). Skipping message." << std::endl;
                                return;
                            }

                            // TODO: Here we need to find a better way how to pass the message to
                            // vsomeip. There doesn't seem to be a public way to just wrap the
               existing
                            // buffer.
                            auto payload_data = message.subspan(VSOMEIP_FULL_HEADER_SIZE);
                            payload->set_data(
                                reinterpret_cast<const vsomeip_v3::byte_t*>(payload_data.data()),
                                payload_data.size());
                            application->notify(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID,
               SAMPLE_EVENT_ID, payload);
                        },
                        max_sample_count);
            */
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        std::cout << "Shutting down SOME/IP daemon..." << std::endl;
        //     }

        application->stop();
    }).detach();

    application->start();
}
