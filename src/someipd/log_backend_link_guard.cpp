/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
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

#include "score/mw/log/irecorder_factory.h"

namespace
{

// Keep a strong symbol reference so static linking retains the recorder factory implementation.
[[maybe_unused]] auto* const kForceRecorderFactoryLink =
    reinterpret_cast<void*>(&score::mw::log::detail::CreateRecorderFactory);

}  // namespace
