
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/s/sharding_task_executor_parameters.h"

#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

using Defaults = mongo::executor::ShardingTaskExecutorDetails;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMaxConnecting,
                                      int,
                                      Defaults::maxConnecting());
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMinSize,
                                      int,
                                      Defaults::minConnections());
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMaxSize,
                                      int,
                                      Defaults::maxConnections());

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolHostTimeoutMS,
                                      int,
                                      Defaults::hostTimeoutMS());
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolRefreshRequirementMS,
                                      int,
                                      Defaults::refreshRequirementMS());
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolRefreshTimeoutMS,
                                      int,
                                      Defaults::refreshTimeoutMS());

} // anonymous

namespace executor {

void ShardingTaskExecutorParameters::load() {
    // We don't set the ConnectionPool's parameters to be the default value in
    // MONGO_EXPORT_STARTUP_SERVER_PARAMETER because it's not guaranteed to be initialized.
    // The following code is a workaround.

    auto minConnections = ShardingTaskExecutorPoolMinSize;
    auto maxConnections = ShardingTaskExecutorPoolMaxSize;
    auto maxConnecting = ShardingTaskExecutorPoolMaxConnecting;

    _minConnections.store(minConnections);
    _maxConnections.store(maxConnections);
    _maxConnecting.store(maxConnecting);

    auto refreshRequirement = ShardingTaskExecutorPoolRefreshRequirementMS;
    auto refreshTimeout = ShardingTaskExecutorPoolRefreshTimeoutMS;
    auto hostTimeout = ShardingTaskExecutorPoolHostTimeoutMS;

    if (refreshRequirement <= refreshTimeout) {
        auto newRefreshTimeout = refreshRequirement - 1;
        warning() << "ShardingTaskExecutorPoolRefreshRequirementMS (" << refreshRequirement
                  << ") set below ShardingTaskExecutorPoolRefreshTimeoutMS (" << refreshTimeout
                  << "). Adjusting ShardingTaskExecutorPoolRefreshTimeoutMS to "
                  << newRefreshTimeout;
        refreshTimeout = newRefreshTimeout;
    }

    if (hostTimeout <= refreshRequirement + refreshTimeout) {
        auto newHostTimeout = refreshRequirement + refreshTimeout + 1;
        warning() << "ShardingTaskExecutorPoolHostTimeoutMS (" << hostTimeout
                  << ") set below ShardingTaskExecutorPoolRefreshRequirementMS ("
                  << refreshRequirement << ") + ShardingTaskExecutorPoolRefreshTimeoutMS ("
                  << refreshTimeout << "). Adjusting ShardingTaskExecutorPoolHostTimeoutMS to "
                  << newHostTimeout;
        hostTimeout = newHostTimeout;
    }

    _refreshRequirementMS.store(refreshRequirement);
    _refreshTimeoutMS.store(refreshTimeout);
    _hostTimeoutMS.store(hostTimeout);
}

}  // namespace executor
}  // namespace mongo
