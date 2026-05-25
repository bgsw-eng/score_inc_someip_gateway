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

#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <vsomeip/defines.hpp>
#include <vsomeip/primitive_types.hpp>
#include <vsomeip/vsomeip.hpp>

#include "score/mw/com/runtime.h"
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

static constexpr vsomeip::major_version_t SOMEIP_MAJOR = 0x01;
static constexpr vsomeip::minor_version_t SOMEIP_MINOR = 0x00;
static constexpr uint16_t WRITE_SERVICE_PORT = 4000;
static constexpr bool WRITE_SERVICE_IS_RELIABLE = false;
static constexpr bool WRITE_SERVICE_USE_MAGIC_COOKIE = false;

using score::someip_gateway::network_service::interfaces::message_transfer::
    SomeipMessageTransferProxy;
using score::someip_gateway::network_service::interfaces::message_transfer::
    SomeipMessageTransferSkeleton;
// Global flag to control application shutdown
static std::atomic<bool> shutdown_requested{false};

// Guards to prevent spawning multiple subscribe threads when ON_AVAILABLE fires repeatedly
static std::atomic<bool> rbc3003_subscribed{false};
static std::atomic<bool> rbc3004_subscribed{false};
static std::atomic<bool> rbc4003_subscribed{false};
static std::atomic<bool> rbc4004_subscribed{false};

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

    score::mw::com::runtime::InitializeRuntime(argc, argv);

    auto runtime = vsomeip::runtime::get();
    auto application = runtime->create_application(someipd_name);
    if (!application->init()) {
        std::cerr << "App init failed" << std::endl;
        return 1;
    }

    std::thread([application]() {
        /*     auto handles =
                 SomeipMessageTransferProxy::FindService(
                     score::mw::com::InstanceSpecifier::Create(std::string("gatewayd/gatewayd_messages"))
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

            // Request events for command services FROM gatewayd.
            // Use the same eventgroup that we later subscribe/offer so vsomeip
            // does not create placeholder subscriptions for an unknown eventgroup.
            std::set<vsomeip::eventgroup_t> eg1{0x0001};
            // application->request_event(0x4003, RBC_INSTANCE_ID, 0x8001, eg1,
            //                            vsomeip::event_type_e::ET_EVENT);
            // application->request_event(0x4004, RBC_INSTANCE_ID, 0x8001, eg1,
            //                           vsomeip::event_type_e::ET_EVENT);
            // std::set<vsomeip::eventgroup_t> eg2_cmd{0x0002};
            // application->request_event(0x4003, RBC_INSTANCE_ID, 0x8002, eg2_cmd,
            //                            vsomeip::event_type_e::ET_EVENT);
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

        // Create mw::com skeleton — publishes received RBC events to SHM for gatewayd
        // Use static so skeleton persists for entire application lifetime
        static auto skeleton_result = SomeipMessageTransferSkeleton::Create(
            score::mw::com::InstanceSpecifier::Create(std::string("someipd/someipd_messages"))
                .value());
        static auto skeleton = std::move(skeleton_result).value();
        static bool skeleton_offered = false;
        if (!skeleton_offered) {
            auto offer_result = skeleton.OfferService();
            if (offer_result.has_value()) {
                std::cout << ">>> Skeleton OfferService() SUCCEEDED" << std::endl;
            } else {
                std::cerr << ">>> Skeleton OfferService() FAILED: "
                          << offer_result.error().Message() << std::endl;
            }
            skeleton_offered = true;
        }

        // Bridge gatewayd -> someipd via IPC SHM channel.
        // This is the write path source for hardcoded gatewayd command injection.
        std::thread([application]() {
            auto proxy_spec_result = score::mw::com::InstanceSpecifier::Create(
                std::string("gatewayd/gatewayd_messages"));
            if (!proxy_spec_result.has_value()) {
                std::cerr << ">>> [IPC] Failed to resolve instance specifier: "
                          << "gatewayd/gatewayd_messages" << std::endl;
                return;
            }

            // Retry until gatewayd registers its SHM service (handles race when someipd starts
            // first).
            std::optional<SomeipMessageTransferProxy> proxy_opt;
            while (!shutdown_requested.load()) {
                auto proxy_handles_result =
                    SomeipMessageTransferProxy::FindService(proxy_spec_result.value());
                if (proxy_handles_result.has_value() && !proxy_handles_result.value().empty()) {
                    auto proxy_result =
                        SomeipMessageTransferProxy::Create(proxy_handles_result.value().front());
                    if (proxy_result.has_value()) {
                        proxy_opt = std::move(proxy_result).value();
                        break;
                    }
                }
                std::cout << ">>> [IPC] gatewayd/gatewayd_messages not yet available, retrying..."
                          << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (!proxy_opt.has_value()) {
                std::cerr << ">>> [IPC] Shutdown before gatewayd service found" << std::endl;
                return;
            }

            auto& proxy = proxy_opt.value();
            std::cout << ">>> [IPC] Connected to gatewayd/gatewayd_messages SHM service"
                      << std::endl;

            // Offer write services only after IPC is ready so CANoe discovery aligns
            // with an operational gatewayd -> someipd forwarding path.
            static bool services_offered = false;
            if (!services_offered) {
                std::cout << ">>> [OFFER] Offering write services (0x4003/0x4004/0x4007)"
                          << std::endl;

                application->offer_service(0x4003, RBC_INSTANCE_ID, SOMEIP_MAJOR, SOMEIP_MINOR);
                std::set<vsomeip::eventgroup_t> lock_cmd_groups{0x0001};

                application->update_service_configuration(
                    0x4003, RBC_INSTANCE_ID, WRITE_SERVICE_PORT, WRITE_SERVICE_IS_RELIABLE,
                    WRITE_SERVICE_USE_MAGIC_COOKIE, true);

                application->offer_event(0x4003, RBC_INSTANCE_ID, 0x8001, lock_cmd_groups,
                                         vsomeip::event_type_e::ET_EVENT,
                                         std::chrono::milliseconds::zero(), false, true, nullptr,
                                         vsomeip::reliability_type_e::RT_UNRELIABLE);
                std::set<vsomeip::eventgroup_t> pos_cmd_groups{0x0002};
                application->offer_event(0x4003, RBC_INSTANCE_ID, 0x8002, pos_cmd_groups,
                                         vsomeip::event_type_e::ET_EVENT,
                                         std::chrono::milliseconds::zero(), false, true, nullptr,
                                         vsomeip::reliability_type_e::RT_UNRELIABLE);

                application->offer_service(0x4004, RBC_INSTANCE_ID, SOMEIP_MAJOR, SOMEIP_MINOR);
                application->update_service_configuration(
                    0x4004, RBC_INSTANCE_ID, WRITE_SERVICE_PORT, WRITE_SERVICE_IS_RELIABLE,
                    WRITE_SERVICE_USE_MAGIC_COOKIE, true);
                std::set<vsomeip::eventgroup_t> cmd_groups{0x0001};
                application->offer_event(0x4004, RBC_INSTANCE_ID, 0x8001, cmd_groups,
                                         vsomeip::event_type_e::ET_EVENT,
                                         std::chrono::milliseconds::zero(), false, true, nullptr,
                                         vsomeip::reliability_type_e::RT_UNRELIABLE);

                application->offer_service(0x4007, RBC_INSTANCE_ID, SOMEIP_MAJOR, SOMEIP_MINOR);

                application->update_service_configuration(
                    0x4007, RBC_INSTANCE_ID, WRITE_SERVICE_PORT, WRITE_SERVICE_IS_RELIABLE,
                    WRITE_SERVICE_USE_MAGIC_COOKIE, true);

                application->offer_event(0x4007, RBC_INSTANCE_ID, 0x8001, cmd_groups,
                                         vsomeip::event_type_e::ET_EVENT,
                                         std::chrono::milliseconds::zero(), false, true, nullptr,
                                         vsomeip::reliability_type_e::RT_UNRELIABLE);

                // application->stop_offer_service(0x4003, RBC_INSTANCE_ID, SOMEIP_MAJOR,
                //                                 SOMEIP_MINOR);
                // application->offer_service(0x4003, RBC_INSTANCE_ID, SOMEIP_MAJOR, SOMEIP_MINOR);
                // application->stop_offer_service(0x4004, RBC_INSTANCE_ID, SOMEIP_MAJOR,
                //                                 SOMEIP_MINOR);
                // application->offer_service(0x4004, RBC_INSTANCE_ID, SOMEIP_MAJOR, SOMEIP_MINOR);
                services_offered = true;
            }

            proxy.message_.SetReceiveHandler([&proxy, application]() {
                proxy.message_.GetNewSamples(
                    [application](auto message_sample) {
                        score::cpp::span<const std::byte> message(message_sample->data,
                                                                  message_sample->size);
                        if (message.size() < VSOMEIP_FULL_HEADER_SIZE + 1) {
                            std::cout
                                << ">>> [IPC] Received short gatewayd frame: " << message.size()
                                << " byte(s)" << std::endl;
                            return;
                        }

                        const auto b0 = static_cast<uint8_t>(message[0]);
                        const auto b1 = static_cast<uint8_t>(message[1]);
                        const auto b2 = static_cast<uint8_t>(message[2]);
                        const auto b3 = static_cast<uint8_t>(message[3]);
                        const uint16_t svc_id = (static_cast<uint16_t>(b0) << 8) | b1;
                        const uint16_t evt_id = (static_cast<uint16_t>(b2) << 8) | b3;
                        const auto payload = message.subspan(VSOMEIP_FULL_HEADER_SIZE);
                        const int v = static_cast<uint8_t>(payload[0]);

                        std::cout << ">>> [IPC->SOMEIP] FRAME RECEIVED [svc=0x" << std::hex
                                  << svc_id << " evt=0x" << evt_id << std::dec << "] value=" << v
                                  << std::endl;

                        // Publish command events on SOME/IP so handlers and subscribers can
                        // observe.
                        if (svc_id == 0x4004 && evt_id == 0x8001) {
                            std::cout << ">>> [WRITE PATH] Publishing gatewayd command to SOME/IP"
                                      << " [svc=0x4004 evt=0x" << std::hex << evt_id << std::dec
                                      << "]" << std::endl;
                            auto payload_obj = vsomeip::runtime::get()->create_payload();
                            std::array<vsomeip::byte_t, 1> out{
                                static_cast<vsomeip::byte_t>(payload[0])};
                            payload_obj->set_data(out.data(), out.size());
                            application->notify(0x4004, RBC_INSTANCE_ID, evt_id, payload_obj);
                        } else if (svc_id == 0x4007 && evt_id == 0x8001) {
                            std::cout << ">>> [WRITE PATH] Publishing gatewayd command to SOME/IP"
                                      << " [svc=0x4007 evt=0x" << std::hex << evt_id << std::dec
                                      << "]" << std::endl;
                            auto payload_obj = vsomeip::runtime::get()->create_payload();
                            std::array<vsomeip::byte_t, 1> out{
                                static_cast<vsomeip::byte_t>(payload[0])};
                            payload_obj->set_data(out.data(), out.size());
                            application->notify(0x4007, RBC_INSTANCE_ID, evt_id, payload_obj);
                        } else if (svc_id == 0x4003 && (evt_id == 0x8001 || evt_id == 0x8002)) {
                            std::cout << ">>> [WRITE PATH] Publishing gatewayd command to SOME/IP"
                                      << " [svc=0x4003 evt=0x" << std::hex << evt_id << std::dec
                                      << "]" << std::endl;
                            auto payload_obj = vsomeip::runtime::get()->create_payload();
                            std::array<vsomeip::byte_t, 1> out{
                                static_cast<vsomeip::byte_t>(payload[0])};
                            payload_obj->set_data(out.data(), out.size());
                            application->notify(0x4003, RBC_INSTANCE_ID, evt_id, payload_obj);
                        }
                    },
                    max_sample_count);
            });
            proxy.message_.Subscribe(max_sample_count);
            std::cout << ">>> [IPC] Subscribed to gatewayd SHM message channel" << std::endl;

            while (!shutdown_requested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }).detach();

        // -------------------------------
        // Register ALL message handlers BEFORE availability handlers.
        // This ensures no event can arrive in the window between on_available firing
        // and the for loop registering later handlers (would be silently dropped).
        // -------------------------------
        for (std::size_t sig_idx = 0; sig_idx < std::size(rbc_signals); ++sig_idx) {
            const auto& sig = rbc_signals[sig_idx];
            application->register_message_handler(
                sig.service_id, RBC_INSTANCE_ID, sig.event_id,
                [sig, sig_idx, &skeleton](const std::shared_ptr<vsomeip::message>& msg) {
                    auto data = msg->get_payload()->get_data();
                    auto len = msg->get_payload()->get_length();
                    if (len < 1) {
                        std::cout << ">>> RBC " << sig.label << ": Payload too short" << std::endl;
                        return;
                    }
                    const int v = static_cast<uint8_t>(data[0]);

                    // Hold mutex only for last-value check/update — not during Send()
                    {
                        std::lock_guard<std::mutex> lock(client_mutex);
                        if (v == rbc_last_value[sig_idx]) {
                            return;
                        }
                        rbc_last_value[sig_idx] = v;
                    }

                    std::cout << ">>> [CANoe->gatewayd] CHANGED [service=0x" << std::hex
                              << sig.service_id << " event=0x" << sig.event_id << std::dec
                              << "] value=" << v << " (" << (v == 0 ? sig.off_label : sig.on_label)
                              << ")" << std::endl;

                    std::cout << ">>> [CANoe->gatewayd] FORWARDING to gatewayd IPC skeleton"
                              << std::endl;

                    // Forward to mw::com skeleton with full 16-byte SOME/IP header
                    // so gatewayd's remote_service_instance can parse it correctly.
                    auto maybe_message = skeleton.message_.Allocate();
                    if (maybe_message.has_value()) {
                        auto message_sample = std::move(maybe_message).value();
                        std::size_t pos = 0;
                        // Bytes 0-1: Service ID
                        message_sample->data[pos++] = static_cast<std::byte>(sig.service_id >> 8);
                        message_sample->data[pos++] = static_cast<std::byte>(sig.service_id & 0xFF);
                        // Bytes 2-3: Method/Event ID
                        message_sample->data[pos++] = static_cast<std::byte>(sig.event_id >> 8);
                        message_sample->data[pos++] = static_cast<std::byte>(sig.event_id & 0xFF);
                        // Bytes 4-7: Length (payload length, 4 bytes big-endian)
                        std::uint32_t payload_len =
                            static_cast<std::uint32_t>(len) + 8U;  // header remainder
                        message_sample->data[pos++] = static_cast<std::byte>(payload_len >> 24);
                        message_sample->data[pos++] =
                            static_cast<std::byte>((payload_len >> 16) & 0xFF);
                        message_sample->data[pos++] =
                            static_cast<std::byte>((payload_len >> 8) & 0xFF);
                        message_sample->data[pos++] = static_cast<std::byte>(payload_len & 0xFF);
                        // Bytes 8-9: Client ID
                        message_sample->data[pos++] = static_cast<std::byte>(0x00);
                        message_sample->data[pos++] = static_cast<std::byte>(0x00);
                        // Bytes 10-11: Session ID
                        message_sample->data[pos++] = static_cast<std::byte>(0x00);
                        message_sample->data[pos++] = static_cast<std::byte>(0x01);
                        // Byte 12: Protocol version
                        message_sample->data[pos++] = static_cast<std::byte>(0x01);
                        // Byte 13: Interface version (major)
                        message_sample->data[pos++] = static_cast<std::byte>(0x01);
                        // Byte 14: Message type (0x02 = NOTIFICATION)
                        message_sample->data[pos++] = static_cast<std::byte>(0x02);
                        // Byte 15: Return code
                        message_sample->data[pos++] = static_cast<std::byte>(0x00);
                        // Bytes 16+: Payload
                        constexpr std::size_t MAX_SIZE = score::someip_gateway::network_service::
                            interfaces::message_transfer::MAX_MESSAGE_SIZE;
                        std::size_t copy_len =
                            std::min(static_cast<std::size_t>(len), MAX_SIZE - pos);
                        std::memcpy(&message_sample->data[pos], data, copy_len);
                        message_sample->size = pos + copy_len;
                        skeleton.message_.Send(std::move(message_sample));
                    }
                });
        }

        // Message handlers for 0x4004 commands FROM gatewayd
        application->register_message_handler(
            0x4003, RBC_INSTANCE_ID, 0x8001, [](const std::shared_ptr<vsomeip::message>& msg) {
                auto data = msg->get_payload()->get_data();
                auto len = msg->get_payload()->get_length();
                if (len < 1) {
                    std::cout << ">>> LOCK CMD: Payload too short" << std::endl;
                    return;
                }
                const int v = static_cast<uint8_t>(data[0]);
                std::cout << ">>> SIGNAL RECEIVED [svc=0x4003 evt=0x8001]" << std::endl;
                std::cout << ">>> [CMD FROM GATEWAYD] CAR LOCK/UNLOCK: "
                          << (v == 0 ? "UNLOCK" : "LOCK") << " (0x" << std::hex << v << std::dec
                          << ")" << std::endl;
            });
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
                        application->subscribe(svc, inst, 0x0002, SOMEIP_MAJOR);
                        application->subscribe(svc, inst, 0x0003, SOMEIP_MAJOR);
                        application->subscribe(svc, inst, 0x0004, SOMEIP_MAJOR);
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
                        application->subscribe(svc, inst, 0x0009, SOMEIP_MAJOR);
                    }).detach();
                } else {
                    std::cout << ">>> RBC 0x3004 unavailable" << std::endl;
                    rbc3004_subscribed.store(false);
                }
            });

        // application->register_availability_handler(
        //     0x4003, RBC_INSTANCE_ID,
        //     [application](vsomeip::service_t svc, vsomeip::instance_t inst, bool available) {
        //         if (available) {
        //             std::cout << ">>> RBC 0x4003 available — subscribing command (0x8002)"
        //                       << std::endl;
        //             if (rbc4003_subscribed.exchange(true)) {
        //                 return;
        //             }
        //             std::thread([application, svc, inst]() {
        //                 std::this_thread::sleep_for(std::chrono::milliseconds(50));
        //                 application->subscribe(svc, inst, 0x0001);
        //                 application->subscribe(svc, inst, 0x0002);  // subscribe to eventgroup
        //             }).detach();
        //         } else {
        //             std::cout << ">>> RBC 0x4003 unavailable" << std::endl;
        //             rbc4003_subscribed.store(false);
        //         }
        //     });

        // Availability handler for 0x4004 commands FROM gatewayd
        //  application->register_availability_handler(
        //      0x4004, RBC_INSTANCE_ID,
        //      [application](vsomeip::service_t svc, vsomeip::instance_t inst, bool available) {
        //          if (available) {
        //              std::cout << ">>> RBC 0x4004 available — subscribing command
        //              (0x8001/0x8002)"
        //                        << std::endl;
        //              if (rbc4004_subscribed.exchange(true)) {
        //                  return;
        //              }
        //              std::thread([application, svc, inst]() {
        //                  std::this_thread::sleep_for(std::chrono::milliseconds(50));
        //                  application->subscribe(svc, inst, 0x0001, SOMEIP_MAJOR, 0x0001);
        //                  //     application->subscribe(svc, inst, 0x0002, SOMEIP_MAJOR);  //
        //                  //     subscribe to eventgroup
        //              }).detach();
        //          } else {
        //              std::cout << ">>> RBC 0x4004 unavailable" << std::endl;
        //              rbc4004_subscribed.store(false);
        //          }
        //      });

        // -------------------------------
        // Step 3 — request_service: triggers SD + availability callback → subscribe.
        // Called once per unique service (not per signal) to avoid ref-count imbalance.
        // -------------------------------
        application->request_service(0x3003, RBC_INSTANCE_ID, SOMEIP_MAJOR, SOMEIP_MINOR);
        application->request_service(0x3004, RBC_INSTANCE_ID, SOMEIP_MAJOR, SOMEIP_MINOR);
        // application->request_service(0x4003, RBC_INSTANCE_ID, SOMEIP_MAJOR,
        //                              SOMEIP_MINOR);  // Request 0x4003 service
        // application->request_service(0x4004, RBC_INSTANCE_ID, SOMEIP_MAJOR,
        //                              SOMEIP_MINOR);  // Request 0x4004 service

        // Subscribe to gatewayd's lamp command IPC events (optional).
        // Some integration setups ship only someipd instance mappings, so do not abort when
        // gatewayd command instance specifiers are missing in mw_com_config.
        auto instance_spec_lock_result = score::mw::com::InstanceSpecifier::Create(
            std::string("gatewayd/application_rbc_lock_unlock_cmd"));
        if (instance_spec_lock_result.has_value()) {
            auto instance_spec_lock = std::move(instance_spec_lock_result).value();
            std::thread([instance_spec_lock]() {
                auto result = score::mw::com::GenericProxy::FindService(instance_spec_lock);
                if (result.has_value()) {
                    auto handles = result.value();
                    if (!handles.empty()) {
                        auto proxy_result = score::mw::com::GenericProxy::Create(handles.front());
                        if (proxy_result.has_value()) {
                            auto proxy = std::move(proxy_result).value();
                            auto& events = proxy.GetEvents();
                            auto evt = events.find("rbc_car_lock_unlock_cmd");
                            if (evt != events.cend()) {
                                evt->second.SetReceiveHandler([]() { /* event received */ });
                                evt->second.Subscribe(10);
                                std::cout << ">>> [IPC] Subscribed to car lock/unlock command from "
                                             "gatewayd"
                                          << std::endl;
                            }
                        }
                    }
                }
            }).detach();
        } else {
            std::cout << ">>> [IPC] Optional instance mapping missing: "
                      << "gatewayd/application_rbc_lock_unlock_cmd (skipping IPC subscribe)"
                      << std::endl;
        }

        auto instance_spec_hazard_result = score::mw::com::InstanceSpecifier::Create(
            std::string("gatewayd/application_rbc_hazard_lamp_cmd"));
        if (instance_spec_hazard_result.has_value()) {
            auto instance_spec_hazard = std::move(instance_spec_hazard_result).value();
            std::thread([instance_spec_hazard]() {
                auto result = score::mw::com::GenericProxy::FindService(instance_spec_hazard);
                if (result.has_value()) {
                    auto handles = result.value();
                    if (!handles.empty()) {
                        auto proxy_result = score::mw::com::GenericProxy::Create(handles.front());
                        if (proxy_result.has_value()) {
                            auto proxy = std::move(proxy_result).value();
                            auto& events = proxy.GetEvents();
                            auto evt = events.find("rbc_hazard_lamp_on_off_cmd");
                            if (evt != events.cend()) {
                                evt->second.SetReceiveHandler([]() { /* event received */ });
                                evt->second.Subscribe(10);
                                std::cout
                                    << ">>> [IPC] Subscribed to hazard lamp command from gatewayd"
                                    << std::endl;
                            }
                        }
                    }
                }
            }).detach();
        } else {
            std::cout << ">>> [IPC] Optional instance mapping missing: "
                      << "gatewayd/application_rbc_hazard_lamp_cmd (skipping IPC subscribe)"
                      << std::endl;
        }

        auto instance_spec_approach_result = score::mw::com::InstanceSpecifier::Create(
            std::string("gatewayd/application_rbc_approach_lamp_cmd"));
        if (instance_spec_approach_result.has_value()) {
            auto instance_spec_approach = std::move(instance_spec_approach_result).value();
            std::thread([instance_spec_approach]() {
                auto result = score::mw::com::GenericProxy::FindService(instance_spec_approach);
                if (result.has_value()) {
                    auto handles = result.value();
                    if (!handles.empty()) {
                        auto proxy_result = score::mw::com::GenericProxy::Create(handles.front());
                        if (proxy_result.has_value()) {
                            auto proxy = std::move(proxy_result).value();
                            auto& events = proxy.GetEvents();
                            auto evt = events.find("rbc_approach_lamp_on_off_cmd");
                            if (evt != events.cend()) {
                                evt->second.SetReceiveHandler([]() { /* event received */ });
                                evt->second.Subscribe(10);
                                std::cout
                                    << ">>> [IPC] Subscribed to approach lamp command from gatewayd"
                                    << std::endl;
                            }
                        }
                    }
                }
            }).detach();
        } else {
            std::cout << ">>> [IPC] Optional instance mapping missing: "
                      << "gatewayd/application_rbc_approach_lamp_cmd (skipping IPC subscribe)"
                      << std::endl;
        }

        auto instance_spec_position_result = score::mw::com::InstanceSpecifier::Create(
            std::string("gatewayd/application_rbc_position_lamp_cmd"));
        if (instance_spec_position_result.has_value()) {
            auto instance_spec_position = std::move(instance_spec_position_result).value();
            std::thread([instance_spec_position]() {
                auto result = score::mw::com::GenericProxy::FindService(instance_spec_position);
                if (result.has_value()) {
                    auto handles = result.value();
                    if (!handles.empty()) {
                        auto proxy_result = score::mw::com::GenericProxy::Create(handles.front());
                        if (proxy_result.has_value()) {
                            auto proxy = std::move(proxy_result).value();
                            auto& events = proxy.GetEvents();
                            auto evt = events.find("rbc_position_lamp_on_off_cmd");
                            if (evt != events.cend()) {
                                evt->second.SetReceiveHandler([]() { /* event received */ });
                                evt->second.Subscribe(10);
                                std::cout
                                    << ">>> [IPC] Subscribed to position lamp command from gatewayd"
                                    << std::endl;
                            }
                        }
                    }
                }
            }).detach();
        } else {
            std::cout << ">>> [IPC] Optional instance mapping missing: "
                      << "gatewayd/application_rbc_position_lamp_cmd (skipping IPC subscribe)"
                      << std::endl;
        }

        // -------------------------------
        // Service Discovery (SD) active
        // -------------------------------
        std::set<vsomeip::eventgroup_t> groups{SAMPLE_EVENTGROUP_ID};

        // Offer own service → SD advertises this service to network
        application->offer_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SOMEIP_MAJOR,
                                   SOMEIP_MINOR);

        // 0x4003/0x4004 write services are offered after IPC readiness above.

        // Offer an event → makes it discoverable
        application->offer_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID, groups);

        // Request own service/event → triggers SD discovery for remote services
        application->request_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SOMEIP_MAJOR,
                                     SOMEIP_MINOR);
        application->request_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID, groups,
                                   vsomeip::event_type_e::ET_EVENT);

        // Subscribe to event group → uses SD to manage subscriptions
        application->subscribe(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENTGROUP_ID,
                               SOMEIP_MAJOR);

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
            application->notify(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID, payload);

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
                application->subscribe(0x3003, RBC_INSTANCE_ID, 0x0002, SOMEIP_MAJOR);
                application->subscribe(0x3003, RBC_INSTANCE_ID, 0x0003, SOMEIP_MAJOR);
                application->subscribe(0x3003, RBC_INSTANCE_ID, 0x0004, SOMEIP_MAJOR);
                application->subscribe(0x3004, RBC_INSTANCE_ID, 0x0009, SOMEIP_MAJOR);
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
