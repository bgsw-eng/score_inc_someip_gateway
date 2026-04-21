/********************************************************************************
* Copyright (c) 2022 Contributors to the Eclipse Foundation
*
* See the NOTICE file(s) distributed with this work for additional
* information regarding copyright ownership.
*
* This program and the accompanying materials are made available under the
* terms of the Apache License 2.0 which is available at
* http://www.apache.org/licenses/LICENSE-2.0
*
* SPDX-License-Identifier: Apache-2.0
********************************************************************************/
/**
 * @file      data_broker_feeder.h
 * @brief     Generic feeder API for KUKSA Data Broker updates.
 */
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "collector_client.h"
#include "sdv/databroker/v1/types.pb.h"

namespace sdv {
namespace broker_feeder {

struct DatapointMetadata {
    std::string name;
    sdv::databroker::v1::DataType data_type;
    sdv::databroker::v1::ChangeType change_type;
    sdv::databroker::v1::Datapoint initial_value;
    std::string description;
};

using DatapointConfiguration = std::vector<DatapointMetadata>;
using DatapointValues = std::unordered_map<std::string, sdv::databroker::v1::Datapoint>;

class DataBrokerFeeder {
   public:
    static std::shared_ptr<DataBrokerFeeder> createInstance(std::shared_ptr<CollectorClient> client,
                                                            DatapointConfiguration&& dpConfig);

    virtual ~DataBrokerFeeder() = default;

    virtual void Run() = 0;

    virtual void Shutdown() = 0;

    virtual void FeedValue(const std::string& name,
                           const sdv::databroker::v1::Datapoint& value) = 0;

    virtual void FeedValues(const DatapointValues& values) = 0;

   protected:
    DataBrokerFeeder() = default;
    DataBrokerFeeder(const DataBrokerFeeder&) = delete;
    DataBrokerFeeder& operator=(const DataBrokerFeeder&) = delete;
};

}  // namespace broker_feeder
}  // namespace sdv
