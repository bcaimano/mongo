/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kConnectionPool

#include "mongo/platform/basic.h"

#include "mongo/executor/connection_pool.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/lru_cache.h"
#include "mongo/util/scopeguard.h"

// One interesting implementation note herein concerns how setup() and
// refresh() are invoked outside of the global lock, but setTimeout is not.
// This implementation detail simplifies mocks, allowing them to return
// synchronously sometimes, whereas having timeouts fire instantly adds little
// value. In practice, dumping the locks is always safe (because we restrict
// ourselves to operations over the connection).

namespace mongo {
namespace executor {

void ConnectionPool::ConnectionInterface::indicateUsed() {
    // It is illegal to attempt to use a connection after calling indicateFailure().
    invariant(_status.isOK() || _status == ConnectionPool::kConnectionStateUnknown);
    _lastUsed = now();
}

void ConnectionPool::ConnectionInterface::indicateSuccess() {
    _status = Status::OK();
}

void ConnectionPool::ConnectionInterface::indicateFailure(Status status) {
    _status = std::move(status);
}

Date_t ConnectionPool::ConnectionInterface::getLastUsed() const {
    return _lastUsed;
}

const Status& ConnectionPool::ConnectionInterface::getStatus() const {
    return _status;
}

void ConnectionPool::ConnectionInterface::resetToUnknown() {
    _status = ConnectionPool::kConnectionStateUnknown;
}

size_t ConnectionPool::ConnectionInterface::getGeneration() const {
    return _generation;
}

// Unsynchronized
class ConnectionPool::PoolClub {
public:
    int minConns = 0;
    int defaultMinConns = 0;

    HostAndPort primary;
    stdx::unordered_set<SpecificPool*> pools;
};

/**
 * A pool for a specific HostAndPort
 *
 * Pools come into existance the first time a connection is requested and
 * go out of existence after hostTimeout passes without any of their
 * connections being used.
 */
class ConnectionPool::SpecificPool final
    : public std::enable_shared_from_this<ConnectionPool::SpecificPool> {
public:
    using Lock = stdx::unique_lock<stdx::mutex>;

    /**
     * Whenever a function enters a specific pool, the function needs to be guarded.
     * The presence of one of these guards will bump a counter on the specific pool
     * which will prevent the pool from removing itself from the map of pools.
     *
     * The complexity comes from the need to hold a lock when writing to the
     * _activeClients param on the specific pool.  Because the code beneath the client needs to lock
     * and unlock the parent mutex (and can leave unlocked), we want to start the client with the
     * lock acquired, move it into the client, then re-acquire to decrement the counter on the way
     * out.
     *
     * This callback also (perhaps overly aggressively) binds a shared pointer to the guard.
     * It is *always* safe to reference the original specific pool in the guarded function object.
     *
     * For a function object of signature:
     * R riskyBusiness(stdx::unique_lock<stdx::mutex>, ArgTypes...);
     *
     * It returns a function object of signature:
     * R safeCallback(ArgTypes...);
     */
    template <typename Callback>
    auto guardCallback(Callback&& cb) {
        return [ cb = std::forward<Callback>(cb), anchor = shared_from_this() ](auto&&... args) {
            stdx::unique_lock<stdx::mutex> lk(anchor->_parent->_mutex);
            ++(anchor->_activeClients);

            ON_BLOCK_EXIT([anchor]() { --(anchor->_activeClients); });

            return cb(lk, std::forward<decltype(args)>(args)...);
        };
    }

    SpecificPool(ConnectionPool* parent, const HostAndPort& hostAndPort);
    ~SpecificPool();

    /**
     * Gets a connection from the specific pool. Sinks a unique_lock from the
     * parent to preserve the lock on _mutex
     */
    Future<ConnectionHandle> getConnection(Milliseconds timeout, Lock& lk);

    /**
     * Gets a connection from the specific pool if a connection is available and there are no
     * outstanding requests.
     */
    boost::optional<ConnectionHandle> tryGetConnection(WithLock);

    /**
     * Triggers the shutdown procedure. This function marks the state as kInShutdown
     * and calls processFailure below with the status provided. This may not immediately
     * delist or destruct this pool. However, both will happen eventually as ConnectionHandles
     * are deleted.
     */
    void triggerShutdown(const Status& status, Lock& lk);

    /**
     * Cascades a failure across existing connections and requests. Invoking
     * this function drops all current connections and fails all current
     * requests with the passed status.
     */
    void processFailure(const Status& status, Lock& lk);

    /**
     * Returns the number of connections currently checked out of the pool.
     */
    size_t inUseConnections(WithLock);

    /**
     * Returns the number of available connections in the pool.
     */
    size_t availableConnections(WithLock);

    /**
     * Returns the number of in progress connections in the pool.
     */
    size_t refreshingConnections(WithLock);

    /**
     * Returns the total number of connections ever created in this pool.
     */
    size_t createdConnections(WithLock);

    /**
     * Returns the total number of connections currently open that belong to
     * this pool. This is the sum of refreshingConnections, availableConnections,
     * and inUseConnections.
     */
    size_t openConnections(WithLock);

    /**
     * Return true if the tags on the specific pool match the passed in tags
     */
    bool matchesTags(WithLock, transport::Session::TagMask tags) const {
        return !!(_tags & tags);
    }

    /**
     * Atomically manipulate the tags in the pool
     */
    void mutateTags(WithLock,
                    const stdx::function<transport::Session::TagMask(transport::Session::TagMask)>&
                        mutateFunc) {
        _tags = mutateFunc(_tags);
    }

    void fassertSSLMode(transport::ConnectSSLMode desired) const {
        if (desired != _sslMode) {
            severe() << "Mixing ssl modes for a single host is not supported";
            fassertFailedNoTrace(51043);
        }
    }

    void setOrCheckSSLMode(transport::ConnectSSLMode desired) {
        if (_created == 0) {
            _sslMode = desired;
            return;
        }
        fassertSSLMode(desired);
    }

    void setController(WithLock, std::shared_ptr<PoolClub> controller) {
        _controller = controller;
        _controller->pools.insert(this);
    }

    void resetController(WithLock lk) {
        // Make an annoymous PoolClub to throw away whenever
        auto controller = std::make_shared<PoolClub>();
        controller->defaultMinConns = _parent->_options.minConnections;
        setController(lk, controller);
    }

    void updateController(WithLock lk) {
        auto& minConns = _controller->minConns;

        /*
        // This is the primary form
        if (_controller->primary == _hostAndPort) {
            minConns = std::max<int>(_controller->defaultMinConns, inUseConnections(lk));
        }
        */

        // This is the minimum form
        minConns = _controller->defaultMinConns;
        for (auto& pool : _controller->pools) {
            minConns = std::max<int>(minConns, pool->inUseConnections(lk));
        }
    }

private:
    using OwnedConnection = std::shared_ptr<ConnectionInterface>;
    using OwnershipPool = stdx::unordered_map<ConnectionInterface*, OwnedConnection>;
    using LRUOwnershipPool = LRUCache<OwnershipPool::key_type, OwnershipPool::mapped_type>;
    using Request = std::pair<Date_t, Promise<ConnectionHandle>>;
    struct RequestComparator {
        bool operator()(const Request& a, const Request& b) {
            return a.first > b.first;
        }
    };

    ConnectionHandle makeHandle(ConnectionInterface* connPtr);

    void addToReady(Lock& lk, OwnedConnection conn);

    void returnConnection(Lock& lk, ConnectionInterface* connection);

    void fulfillRequests(Lock& lk);

    void finishRefresh(Lock& lk, ConnectionInterface* connPtr, Status status);

    void spawnConnections(Lock& lk);

    // This internal helper is used both by tryGet and by fulfillRequests and differs in that it
    // skips some bookkeeping that the other callers do on their own
    boost::optional<ConnectionHandle> tryGetInternal(WithLock);

    template <typename OwnershipPoolType>
    typename OwnershipPoolType::mapped_type takeFromPool(
        OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr);

    OwnedConnection takeFromProcessingPool(ConnectionInterface* connection);

    void updateStateInLock();

    void checkShutdown(Lock& lk);

private:
    ConnectionPool* const _parent;

    const HostAndPort _hostAndPort;

    transport::ConnectSSLMode _sslMode;
    std::shared_ptr<PoolClub> _controller;

    LRUOwnershipPool _readyPool;
    OwnershipPool _processingPool;
    OwnershipPool _droppedProcessingPool;
    OwnershipPool _checkedOutPool;

    std::vector<Request> _requests;

    std::shared_ptr<TimerInterface> _requestTimer;
    Date_t _requestTimerExpiration;
    size_t _activeClients;
    size_t _generation;

    size_t _created;

    transport::Session::TagMask _tags = transport::Session::kPending;

    /**
     * The current state of the pool
     *
     * The pool begins in a running state. Moves to idle when no requests
     * are pending and no connections are checked out. It finally enters
     * shutdown after hostTimeout has passed (and waits there for current
     * refreshes to process out).
     *
     * At any point a new request sets the state back to running and
     * restarts all timers.
     */
    enum class State {
        // The pool is active
        kRunning,

        // No current activity, waiting for hostTimeout to pass
        kIdle,

        // hostTimeout is passed, we're waiting for ConnectionPool to let us die
        kHostTimedOut,

        // ConnectionPool has told us to die, we're waiting for any processing
        // connections to finish before shutting down
        kInShutdown,
    };

    State _state;
};

constexpr Milliseconds ConnectionPool::kDefaultHostTimeout;
size_t const ConnectionPool::kDefaultMaxConns = std::numeric_limits<size_t>::max();
size_t const ConnectionPool::kDefaultMinConns = 1;
size_t const ConnectionPool::kDefaultMaxConnecting = std::numeric_limits<size_t>::max();
constexpr Milliseconds ConnectionPool::kDefaultRefreshRequirement;
constexpr Milliseconds ConnectionPool::kDefaultRefreshTimeout;

const Status ConnectionPool::kConnectionStateUnknown =
    Status(ErrorCodes::InternalError, "Connection is in an unknown state");

ConnectionPool::ConnectionPool(Options options)
    : _options(std::move(options)),
      _factory(_options.factory),
      _executor(_options.executor),
      _manager(options.egressTagCloserManager) {
    invariant(!_options.name.empty());
    invariant(_executor);

    if (_manager) {
        _manager->add(this);
    }
}

ConnectionPool::~ConnectionPool() {
    // If we're currently destroying the service context the _manager is already deleted and this
    // pointer dangles. No need for cleanup in that case.
    if (hasGlobalServiceContext() && _manager) {
        _manager->remove(this);
    }

    shutdown();
}

void ConnectionPool::shutdown() {
    _factory->shutdown();

    // Grab all current pools (under the lock)
    auto pools = [&] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        return _pools;
    }();

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    for (const auto& pair : pools) {
        pair.second->triggerShutdown(
            Status(ErrorCodes::ShutdownInProgress, "Shutting down the connection pool"), lk);
    }
}

void ConnectionPool::dropConnections(const HostAndPort& hostAndPort) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    auto pool = iter->second;
    pool->processFailure(Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"),
                         lk);
}

void ConnectionPool::dropConnections(transport::Session::TagMask tags) {
    // Grab all current pools (under the lock)
    auto pools = [&] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        return _pools;
    }();

    for (const auto& pair : pools) {
        auto& pool = pair.second;

        stdx::unique_lock<stdx::mutex> lk(_mutex);
        if (pool->matchesTags(lk, tags))
            continue;

        pool->processFailure(
            Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"), lk);
    }
}

void ConnectionPool::mutateTags(
    const HostAndPort& hostAndPort,
    const stdx::function<transport::Session::TagMask(transport::Session::TagMask)>& mutateFunc) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    auto pool = iter->second;
    pool->mutateTags(lk, mutateFunc);
}

auto ConnectionPool::_getPoolClub(WithLock, const std::string& replSet)
    -> std::shared_ptr<PoolClub> {
    auto& poolClub = _poolClubs[replSet];
    if (!poolClub) {
        poolClub = std::make_shared<PoolClub>();
        poolClub->defaultMinConns = _options.minConnections;
    }

    return poolClub;
}

void ConnectionPool::handleConfig(const ConnectionString& str) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto poolClub = _getPoolClub(lk, str.getSetName());

    // Save what used to be the club for later
    auto oldPools = std::exchange(poolClub->pools, {});

    // Add in each pool that is in the new config, the majority are probably the same
    for (auto& host : str.getServers()) {
        auto pool = _getPool(lk, host);

        pool->setController(lk, poolClub);
        oldPools.erase(pool.get());
    }

    // Reset the controller for anything that's left
    for (auto& pool : oldPools) {
        pool->resetController(lk);
    }

    // Reset the club state
    poolClub->minConns = poolClub->defaultMinConns;
    for (auto& pool : poolClub->pools) {
        pool->updateController(lk);
    }
}

void ConnectionPool::handlePrimary(const std::string& replSet, const HostAndPort& host) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto club = _getPoolClub(lk, replSet);
    if (club->primary != host) {
        club->primary = host;

        auto pool = _tryGetPool(lk, host);
        pool->updateController(lk);
    }
}

void ConnectionPool::get_forTest(const HostAndPort& hostAndPort,
                                 Milliseconds timeout,
                                 GetConnectionCallback cb) {
    return get(hostAndPort, transport::kGlobalSSLMode, timeout).getAsync(std::move(cb));
}

auto ConnectionPool::_tryGetPool(WithLock, const HostAndPort& hostAndPort)
    -> std::shared_ptr<SpecificPool> {
    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end()) {
        return {};
    }

    const auto& pool = iter->second;
    invariant(pool);
    return pool;
}

auto ConnectionPool::_getPool(WithLock lk, const HostAndPort& hostAndPort)
    -> std::shared_ptr<SpecificPool> {
    auto pool = _tryGetPool(lk, hostAndPort);
    if (pool) {
        return pool;
    }

    pool = std::make_shared<SpecificPool>(this, hostAndPort);

    pool->resetController(lk);
    _pools[hostAndPort] = pool;

    return pool;
}


boost::optional<ConnectionPool::ConnectionHandle> ConnectionPool::tryGet(
    const HostAndPort& hostAndPort, transport::ConnectSSLMode sslMode) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto pool = _tryGetPool(lk, hostAndPort);
    if (!pool)
        return boost::none;

    pool->fassertSSLMode(sslMode);
    return pool->tryGetConnection(lk);
}

Future<ConnectionPool::ConnectionHandle> ConnectionPool::get(const HostAndPort& hostAndPort,
                                                             transport::ConnectSSLMode sslMode,
                                                             Milliseconds timeout) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto pool = _getPool(lk, hostAndPort);
    pool->setOrCheckSSLMode(sslMode);
    return pool->getConnection(timeout, lk);
}

void ConnectionPool::appendConnectionStats(ConnectionPoolStats* stats) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    for (const auto& kv : _pools) {
        HostAndPort host = kv.first;

        auto& pool = kv.second;
        ConnectionStatsPer hostStats{pool->inUseConnections(lk),
                                     pool->availableConnections(lk),
                                     pool->createdConnections(lk),
                                     pool->refreshingConnections(lk)};
        stats->updateStatsForHost(_options.name, host, hostStats);
    }
}

size_t ConnectionPool::getNumConnectionsPerHost(const HostAndPort& hostAndPort) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto iter = _pools.find(hostAndPort);
    if (iter != _pools.end()) {
        return iter->second->openConnections(lk);
    }

    return 0;
}

ConnectionPool::SpecificPool::SpecificPool(ConnectionPool* parent, const HostAndPort& hostAndPort)
    : _parent(parent),
      _hostAndPort(hostAndPort),
      _readyPool(std::numeric_limits<size_t>::max()),
      _requestTimer(parent->_factory->makeTimer()),
      _activeClients(0),
      _generation(0),
      _created(0),
      _state(State::kRunning) {}

ConnectionPool::SpecificPool::~SpecificPool() {
    DESTRUCTOR_GUARD(_requestTimer->cancelTimeout();)

    invariant(_requests.empty());
    invariant(_checkedOutPool.empty());
}

size_t ConnectionPool::SpecificPool::inUseConnections(WithLock) {
    return _checkedOutPool.size();
}

size_t ConnectionPool::SpecificPool::availableConnections(WithLock) {
    return _readyPool.size();
}

size_t ConnectionPool::SpecificPool::refreshingConnections(WithLock) {
    return _processingPool.size();
}

size_t ConnectionPool::SpecificPool::createdConnections(WithLock) {
    return _created;
}

size_t ConnectionPool::SpecificPool::openConnections(WithLock) {
    return _checkedOutPool.size() + _readyPool.size() + _processingPool.size();
}

// Unlock method
Future<ConnectionPool::ConnectionHandle> ConnectionPool::SpecificPool::getConnection(
    Milliseconds timeout, Lock& lk) {
    invariant(_state != State::kInShutdown);

    // If we have a connection ready, just hand it out
    auto conn = tryGetInternal(lk);
    if (conn) {
        return Future<ConnectionPool::ConnectionHandle>::makeReady(std::move(*conn));
    }

    // We don't have a connection ready, so we mark the request as a promise,
    // send off some connection requests, and hand out a future
    auto pf = makePromiseFuture<ConnectionHandle>();

    if (timeout < Milliseconds(0) || timeout > _parent->_options.refreshTimeout) {
        timeout = _parent->_options.refreshTimeout;
    }

    const auto expiration = _parent->_factory->now() + timeout;

    _requests.push_back(make_pair(expiration, std::move(pf.promise)));
    std::push_heap(begin(_requests), end(_requests), RequestComparator{});

    updateStateInLock();

    lk.unlock();
    _parent->_executor->schedule(guardCallback([this](auto& lk) { spawnConnections(lk); }));
    lk.lock();

    return std::move(pf.future);
}

auto ConnectionPool::SpecificPool::tryGetConnection(WithLock lk)
    -> boost::optional<ConnectionHandle> {
    invariant(_state != State::kInShutdown);

    if (_requests.size()) {
        return boost::none;
    }

    auto conn = tryGetInternal(lk);

    updateStateInLock();

    return conn;
}

auto ConnectionPool::SpecificPool::tryGetInternal(WithLock lk)
    -> boost::optional<ConnectionHandle> {
    while (_readyPool.size()) {
        // _readyPool is an LRUCache, so its begin() object is the MRU item.
        auto iter = _readyPool.begin();

        // Grab the connection and cancel its timeout
        auto conn = std::move(iter->second);
        _readyPool.erase(iter);
        conn->cancelTimeout();

        if (!conn->isHealthy()) {
            log() << "dropping unhealthy pooled connection to " << conn->getHostAndPort();

            // Drop the bad connection via scoped destruction and retry
            continue;
        }

        auto connPtr = conn.get();

        // check out the connection
        _checkedOutPool[connPtr] = std::move(conn);

        // pass it to the user
        connPtr->resetToUnknown();
        return makeHandle(connPtr);
    }

    return boost::none;
}

// Unlock method
void ConnectionPool::SpecificPool::returnConnection(Lock& lk, ConnectionInterface* connPtr) {
    auto needsRefreshTP = connPtr->getLastUsed() + _parent->_options.refreshRequirement;

    auto conn = takeFromPool(_checkedOutPool, connPtr);
    invariant(conn);

    updateStateInLock();

    if (conn->getGeneration() != _generation) {
        // If the connection is from an older generation, just return.
        return;
    }

    if (!conn->getStatus().isOK()) {
        // TODO: alert via some callback if the host is bad
        log() << "Ending connection to host " << _hostAndPort << " due to bad connection status; "
              << openConnections(lk) << " connections to that host remain open";
        return;
    }

    auto now = _parent->_factory->now();
    if (needsRefreshTP <= now) {
        // If we need to refresh this connection

        if (int(_readyPool.size() + _processingPool.size() + _checkedOutPool.size()) >=
            _controller->minConns) {
            // If we already have minConnections, just let the connection lapse
            log() << "Ending idle connection to host " << _hostAndPort
                  << " because the pool meets constraints; " << openConnections(lk)
                  << " connections to that host remain open";
            return;
        }

        _processingPool[connPtr] = std::move(conn);

        // Unlock in case refresh can occur immediately
        lk.unlock();
        connPtr->refresh(
            _parent->_options.refreshTimeout,
            guardCallback([this](Lock& lk, ConnectionInterface* connPtr, Status status) {
                return finishRefresh(lk, connPtr, status);
            }));
        lk.lock();
    } else {
        // If it's fine as it is, just put it in the ready queue
        addToReady(lk, std::move(conn));
    }

    updateStateInLock();
}


inline ConnectionPool::ConnectionHandle ConnectionPool::SpecificPool::makeHandle(
    ConnectionInterface* connPtr) {
    auto deleter = [this](ConnectionInterface* conn) {
        _parent->_executor->schedule(guardCallback([&](auto& lk) { returnConnection(lk, conn); }));
    };
    return ConnectionHandle(connPtr, std::move(deleter));
}

// Adds a live connection to the ready pool
void ConnectionPool::SpecificPool::addToReady(Lock& lk, OwnedConnection conn) {
    auto connPtr = conn.get();

    // This makes the connection the new most-recently-used connection.
    _readyPool.add(connPtr, std::move(conn));

    // Our strategy for refreshing connections is to check them out and
    // immediately check them back in (which kicks off the refresh logic in
    // returnConnection
    connPtr->setTimeout(_parent->_options.refreshRequirement,
                        guardCallback([this, connPtr](auto& lk) {
                            auto conn = takeFromPool(_readyPool, connPtr);

                            // We've already been checked out. We don't need to refresh
                            // ourselves.
                            if (!conn)
                                return;

                            // If we're in shutdown, we don't need to refresh connections
                            if (_state == State::kInShutdown)
                                return;

                            _checkedOutPool[connPtr] = std::move(conn);

                            connPtr->indicateSuccess();

                            returnConnection(lk, connPtr);
                        }));

    // If we currently have outstanding requests and nothing scheduled, try to fill out
    fulfillRequests(lk);
}

// Sets state to shutdown and kicks off the failure protocol to tank existing connections
void ConnectionPool::SpecificPool::triggerShutdown(const Status& status, Lock& lk) {
    _state = State::kInShutdown;
    _droppedProcessingPool.clear();
    processFailure(status, lk);
}

// Drop connections and fail all requests
// Unlock method
void ConnectionPool::SpecificPool::processFailure(const Status& status, Lock& lk) {
    // Bump the generation so we don't reuse any pending or checked out
    // connections
    _generation++;

    if (!_readyPool.empty() || !_processingPool.empty()) {
        auto severity = MONGO_GET_LIMITED_SEVERITY(_hostAndPort, Seconds{1}, 0, 2);
        LOG(severity) << "Dropping all pooled connections to " << _hostAndPort << " due to "
                      << redact(status);
    }

    // When a connection enters the ready pool, its timer is set to eventually refresh the
    // connection. This requires a lifetime extension of the specific pool because the connection
    // timer is tied to the lifetime of the connection, not the pool. That said, we can destruct
    // all of the connections and thus timers of which we have ownership.
    // In short, clearing the ready pool helps the SpecificPool drain.
    _readyPool.clear();

    // Migrate processing connections to the dropped pool
    for (auto&& x : _processingPool) {
        if (_state != State::kInShutdown) {
            // If we're just dropping the pool, we can reuse them later
            _droppedProcessingPool[x.first] = std::move(x.second);
        }
    }
    _processingPool.clear();

    // Move the requests out so they aren't visible
    // in other threads
    decltype(_requests) requestsToFail;
    {
        using std::swap;
        swap(requestsToFail, _requests);
    }

    // Update state to reflect the lack of requests
    updateStateInLock();

    // Drop the lock and process all of the requests
    // with the same failed status
    lk.unlock();

    for (auto& request : requestsToFail) {
        request.second.setError(status);
    }

    lk.lock();
}

// fulfills as many outstanding requests as possible
// Unlock method
void ConnectionPool::SpecificPool::fulfillRequests(Lock& lk) {
    while (!_requests.empty()) {
        // Caution: If this returns with a value, it's important that we not throw until we've
        // emplaced the promise (as returning a connection would attempt to take the lock and would
        // deadlock).
        //
        // None of the heap manipulation code throws, but it's something to keep in mind.
        auto conn = tryGetInternal(lk);

        if (!conn)
            break;

        // Grab the request and callback
        auto promise = std::move(_requests.front().second);
        std::pop_heap(begin(_requests), end(_requests), RequestComparator{});
        _requests.pop_back();

        lk.unlock();
        promise.emplaceValue(std::move(*conn));
        lk.lock();

        updateStateInLock();
    }

    // Make sure that our controller knows the current state of this pool
    updateController(lk);

    for (auto& pool : _controller->pools) {
        pool->spawnConnections(lk);
    }
}

void ConnectionPool::SpecificPool::finishRefresh(Lock& lk,
                                                 ConnectionInterface* connPtr,
                                                 Status status) {
    auto conn = takeFromProcessingPool(connPtr);

    // If we're in shutdown, we don't need refreshed connections
    if (_state == State::kInShutdown) {
        return;
    }

    // Spawn connections for most cases
    auto spawnGuard = makeGuard([this, &lk]() { spawnConnections(lk); });

    // If the connection refreshed successfully, throw it back in
    // the ready pool
    if (status.isOK()) {
        // If the host and port were dropped, let this lapse
        if (conn->getGeneration() != _generation) {
            return;
        }

        spawnGuard.dismiss();
        addToReady(lk, std::move(conn));
        return;
    }

    // If we've exceeded the time limit, start a new connect,
    // rather than failing all operations.  We do this because the
    // various callers have their own time limit which is unrelated
    // to our internal one.
    if (status.code() == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
        log() << "Pending connection to host " << _hostAndPort
              << " did not complete within the connection timeout,"
              << " retrying with a new connection;" << openConnections(lk)
              << " connections to that host remain open";
        return;
    }

    // Otherwise pass the failure on through
    spawnGuard.dismiss();
    processFailure(status, lk);
}

// spawn enough connections to satisfy open requests and minpool, while
// honoring maxpool
// Unlock method
void ConnectionPool::SpecificPool::spawnConnections(Lock& lk) {
    // We want minConnections <= outstanding requests <= maxConnections
    auto target = [&] {
        return std::max<unsigned int>(
            _controller->minConns,
            std::min(_requests.size() + _checkedOutPool.size(), _parent->_options.maxConnections));
    };

    // While all of our inflight connections are less than our target
    while ((_state != State::kInShutdown) &&
           (_readyPool.size() + _processingPool.size() + _checkedOutPool.size() < target()) &&
           (_processingPool.size() < _parent->_options.maxConnecting)) {
        if (_readyPool.empty() && _processingPool.empty()) {
            auto severity = MONGO_GET_LIMITED_SEVERITY(_hostAndPort, Seconds{1}, 0, 2);
            LOG(severity) << "Connecting to " << _hostAndPort;
        }

        OwnedConnection handle;
        try {
            // make a new connection and put it in processing
            handle = _parent->_factory->makeConnection(_hostAndPort, _sslMode, _generation);
        } catch (std::system_error& e) {
            severe() << "Failed to construct a new connection object: " << e.what();
            fassertFailed(40336);
        }

        _processingPool[handle.get()] = handle;

        ++_created;

        // Run the setup callback
        lk.unlock();
        handle->setup(_parent->_options.refreshTimeout,
                      guardCallback([this](auto& lk, ConnectionInterface* connPtr, Status status) {
                          return finishRefresh(lk, connPtr, status);
                      }));
        // Note that this assumes that the refreshTimeout is sound for the
        // setupTimeout

        lk.lock();
    }
}

void ConnectionPool::SpecificPool::checkShutdown(Lock& lk) {
    for (auto& pool : _controller->pools) {
        if (pool->_state != State::kHostTimedOut)
            return;
    }

    // We made it through, shut down everything
    for (auto& pool : _controller->pools) {
        pool->triggerShutdown(
            Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                   "Connection pool has been idle for longer than the host timeout"),
            lk);
    }
}

template <typename OwnershipPoolType>
typename OwnershipPoolType::mapped_type ConnectionPool::SpecificPool::takeFromPool(
    OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr) {
    auto iter = pool.find(connPtr);
    if (iter == pool.end())
        return typename OwnershipPoolType::mapped_type();

    auto conn = std::move(iter->second);
    pool.erase(iter);
    return conn;
}

ConnectionPool::SpecificPool::OwnedConnection ConnectionPool::SpecificPool::takeFromProcessingPool(
    ConnectionInterface* connPtr) {
    auto conn = takeFromPool(_processingPool, connPtr);
    if (conn) {
        invariant(_state != State::kInShutdown);
        return conn;
    }

    return takeFromPool(_droppedProcessingPool, connPtr);
}


// Updates our state and manages the request timer
void ConnectionPool::SpecificPool::updateStateInLock() {
    if (_state == State::kInShutdown) {
        // If we're in shutdown, there is nothing to update. Our clients are all gone.
        if (_processingPool.empty() && !_activeClients) {
            // If we have no more clients that require access to us, delist from the parent pool
            LOG(2) << "Delisting connection pool for " << _hostAndPort;
            _controller->pools.erase(this);
            _parent->_pools.erase(_hostAndPort);
        }
        return;
    }

    if (_requests.size()) {
        // We have some outstanding requests, we're live

        // If we were already running and the timer is the same as it was
        // before, nothing to do
        if (_state == State::kRunning && _requestTimerExpiration == _requests.front().first)
            return;

        _state = State::kRunning;

        _requestTimer->cancelTimeout();

        _requestTimerExpiration = _requests.front().first;

        auto timeout = _requests.front().first - _parent->_factory->now();

        // We set a timer for the most recent request, then invoke each timed
        // out request we couldn't service
        _requestTimer->setTimeout(
            timeout, guardCallback([this](auto& lk) {
                auto now = _parent->_factory->now();

                while (_requests.size()) {
                    auto& x = _requests.front();

                    if (x.first <= now) {
                        auto promise = std::move(x.second);
                        std::pop_heap(begin(_requests), end(_requests), RequestComparator{});
                        _requests.pop_back();

                        lk.unlock();
                        promise.setError(Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                                                "Couldn't get a connection within the time limit"));
                        lk.lock();
                    } else {
                        break;
                    }
                }

                updateStateInLock();
            }));
    } else if (_checkedOutPool.size()) {
        // If we have no requests, but someone's using a connection, we just
        // hang around until the next request or a return

        _requestTimer->cancelTimeout();
        _state = State::kRunning;
        _requestTimerExpiration = _requestTimerExpiration.max();
    } else {
        // If we don't have any live requests and no one has checked out connections

        // If we used to be idle, just bail
        if (_state == State::kIdle)
            return;

        _state = State::kIdle;

        _requestTimer->cancelTimeout();

        _requestTimerExpiration = _parent->_factory->now() + _parent->_options.hostTimeout;

        auto timeout = _parent->_options.hostTimeout;

        // Set the shutdown timer, this gets reset on any request
        _requestTimer->setTimeout(timeout, [ this, anchor = shared_from_this() ]() {
            stdx::unique_lock<stdx::mutex> lk(anchor->_parent->_mutex);
            if (_state != State::kIdle)
                return;

            _state = State::kHostTimedOut;
            checkShutdown(lk);
        });
    }
}

}  // namespace executor
}  // namespace mongo
