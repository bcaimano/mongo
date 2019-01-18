
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

#include <limits>
#include <memory>

#include "mongo/platform/atomic_word.h"

namespace mongo {
namespace executor {

struct ConnectionPoolParameters {
    ConnectionPoolParameters() = default;
    virtual ~ConnectionPoolParameters() = default;

    ConnectionPoolParameters(const ConnectionPoolParameters&) = delete;
    ConnectionPoolParameters& operator=(const ConnectionPoolParameters&) = delete;

    ConnectionPoolParameters(ConnectionPoolParameters&&) = default;
    ConnectionPoolParameters& operator=(ConnectionPoolParameters&&) = default;

    /**
     * The minimum number of connections to keep alive while the pool is in
     * operation
     */
    virtual int32_t minConnections() const = 0;

    /**
     * The maximum number of connections to spawn for a host. This includes
     * pending connections in setup and connections checked out of the pool
     * as well as the obvious live connections in the pool.
     */
    virtual int32_t maxConnections() const = 0;

    /**
     * The maximum number of processing connections for a host.  This includes pending
     * connections in setup/refresh. It's designed to rate limit connection storms rather than
     * steady state processing (as maxConnections does).
     */
    virtual int32_t maxConnecting() const = 0;

    /**
     * Amount of time to wait before timing out a refresh attempt
     */
    virtual int32_t refreshTimeoutMS() const = 0;

    /**
     * Amount of time a connection may be idle before it cannot be returned
     * for a user request and must instead be checked out and refreshed
     * before handing to a user.
     */
    virtual int32_t refreshRequirementMS() const = 0;

    /**
     * Amount of time to keep a specific pool around without any checked
     * out connections or new requests
     */
    virtual int32_t hostTimeoutMS() const = 0;
};

template <typename DetailsT>
struct ConnectionPoolParametersAtomic : public ConnectionPoolParameters {
public:
    using Details = DetailsT;

    ConnectionPoolParametersAtomic() : ConnectionPoolParameters{} {}
    virtual ~ConnectionPoolParametersAtomic() = default;

    int32_t minConnections() const override {
        return _minConnections.loadRelaxed();
    }
    int32_t maxConnections() const override {
        return _maxConnections.loadRelaxed();
    }
    int32_t maxConnecting() const override {
        return _maxConnecting.loadRelaxed();
    }

    int32_t refreshTimeoutMS() const override {
        return _refreshTimeoutMS.loadRelaxed();
    }
    int32_t refreshRequirementMS() const override {
        return _refreshRequirementMS.loadRelaxed();
    }
    int32_t hostTimeoutMS() const override {
        return _hostTimeoutMS.loadRelaxed();
    }

protected:
    AtomicWord<int32_t> _minConnections{Details::minConnections()};
    AtomicWord<int32_t> _maxConnections{Details::maxConnections()};
    AtomicWord<int32_t> _maxConnecting{Details::maxConnecting()};

    AtomicWord<int32_t> _refreshTimeoutMS{Details::refreshTimeoutMS()};
    AtomicWord<int32_t> _refreshRequirementMS{Details::refreshRequirementMS()};
    AtomicWord<int32_t> _hostTimeoutMS{Details::hostTimeoutMS()};
};

struct ConnectionPoolParametersDefaultDetails {
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
        return std::numeric_limits<int32_t>::max();
    }
};

class ConnectionPoolParametersDefault
    : public ConnectionPoolParametersAtomic<ConnectionPoolParametersDefaultDetails> {
public:
    static std::shared_ptr<ConnectionPoolParametersDefault> global() {
        static auto params = std::make_shared<ConnectionPoolParametersDefault>();
        return params;
    }
};

}  // namespace executor
}  // namespace mongo
