
/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/executor/connection_pool_parameters.h"

namespace mongo {
namespace executor {

struct ShardingTaskExecutorDetails {
    static constexpr int32_t hostTimeoutMS() {
        // 5min
        return 300000;
    }
    static constexpr int32_t refreshRequirementMS() {
        // 1min
        return 60000;
    }
    static constexpr int32_t refreshTimeoutMS() {
        // 20secs
        return 20000;
    }

    static constexpr int32_t minConnections() {
        return 1;
    }
    static constexpr int32_t maxConnections() {
        return std::numeric_limits<int32_t>::max();
    }
    static constexpr int32_t maxConnecting() {
        // By default, limit us to two concurrent pending connection attempts
        // in any one pool. Since pools are currently per-cpu, we still may
        // have something like 64 concurrent total connection attempts on a
        // modestly sized system. We could set it to one, but that seems too
        // restrictive.
        return 2ll;
    }
};

class ShardingTaskExecutorParameters
    : public ConnectionPoolParametersAtomic<ShardingTaskExecutorDetails> {
public:
    static std::shared_ptr<ShardingTaskExecutorParameters> global() {
        static auto params = std::make_shared<ShardingTaskExecutorParameters>();
        return params;
    }

    void load();
};

}  // namespace executor
}  // namespace mongo
