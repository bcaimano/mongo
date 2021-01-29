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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/service_context.h"

#include <list>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/default_baton.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/system_tick_source.h"
#include <iostream>

namespace mongo {
namespace {

ServiceContext* globalServiceContext = nullptr;

AtomicWord<int> _numCurrentOps{0};

}  // namespace

LockedClient::LockedClient(Client* client) : _lk{*client}, _client{client} {}

bool hasGlobalServiceContext() {
    return globalServiceContext;
}

ServiceContext* getGlobalServiceContext() {
    fassert(17508, globalServiceContext);
    return globalServiceContext;
}

ServiceContext* getCurrentServiceContext() {
    auto client = Client::getCurrent();
    if (client) {
        return client->getServiceContext();
    }

    return nullptr;
}

void setGlobalServiceContext(ServiceContext::UniqueServiceContext&& serviceContext) {
    if (globalServiceContext) {
        // Make sure that calling getGlobalServiceContext() during the destructor results in
        // nullptr. Decorations might try and do this.
        ServiceContext::UniqueServiceContext oldServiceContext{globalServiceContext};
        globalServiceContext = nullptr;
    }

    globalServiceContext = serviceContext.release();
}

ServiceContext::ServiceContext()
    : _tickSource(std::make_unique<SystemTickSource>()),
      _fastClockSource(std::make_unique<SystemClockSource>()),
      _preciseClockSource(std::make_unique<SystemClockSource>()) {}

ServiceContext::~ServiceContext() {
    stdx::lock_guard<Latch> lk(_mutex);
    for (const auto& client : _clients) {
        LOGV2_ERROR(23828,
                    "{client} exists while destroying {serviceContext}",
                    "Non-empty client list when destroying service context",
                    "client"_attr = client->desc(),
                    "serviceContext"_attr = reinterpret_cast<uint64_t>(this));
    }
    invariant(_clients.empty());
}

ServiceContext::UniqueClient ServiceContext::makeClient(std::string desc,
                                                        transport::SessionHandle session) {
    std::unique_ptr<Client> client(new Client(std::move(desc), this, std::move(session)));
    client->onCreate();
    {
        stdx::lock_guard<Latch> lk(_mutex);
        invariant(_clients.insert(client.get()).second);
    }
    return UniqueClient(client.release());
}

void ServiceContext::setPeriodicRunner(std::unique_ptr<PeriodicRunner> runner) {
    invariant(!_runner);
    _runner = std::move(runner);
}

PeriodicRunner* ServiceContext::getPeriodicRunner() const {
    return _runner.get();
}

transport::TransportLayer* ServiceContext::getTransportLayer() const {
    return _transportLayer.get();
}

ServiceEntryPoint* ServiceContext::getServiceEntryPoint() const {
    return _serviceEntryPoint.get();
}

void ServiceContext::setStorageEngine(std::unique_ptr<StorageEngine> engine) {
    invariant(engine);
    invariant(!_storageEngine);
    _storageEngine = std::move(engine);
}

void ServiceContext::setOpObserver(std::unique_ptr<OpObserver> opObserver) {
    _opObserver = std::move(opObserver);
}

void ServiceContext::setTickSource(std::unique_ptr<TickSource> newSource) {
    _tickSource = std::move(newSource);
}

void ServiceContext::setFastClockSource(std::unique_ptr<ClockSource> newSource) {
    _fastClockSource = std::move(newSource);
}

void ServiceContext::setPreciseClockSource(std::unique_ptr<ClockSource> newSource) {
    _preciseClockSource = std::move(newSource);
}

void ServiceContext::setServiceEntryPoint(std::unique_ptr<ServiceEntryPoint> sep) {
    _serviceEntryPoint = std::move(sep);
}

void ServiceContext::setTransportLayer(std::unique_ptr<transport::TransportLayer> tl) {
    _transportLayer = std::move(tl);
}

void ServiceContext::ClientDeleter::operator()(Client* client) const {
    ServiceContext* const service = client->getServiceContext();
    {
        stdx::lock_guard<Latch> lk(service->_mutex);
        invariant(service->_clients.erase(client));
    }
    client->onDestroy();
    delete client;
}

ServiceContext::UniqueOperationContext ServiceContext::makeOperationContext(Client* client) {
    auto opCtx = std::make_unique<OperationContext>(client, _nextOpId.fetchAndAdd(1));
    if (client->session()) {
        _numCurrentOps.addAndFetch(1);
    }

    opCtx->onCreate();
    if (!opCtx->lockState()) {
        opCtx->setLockState(std::make_unique<LockerNoop>());
    }
    if (!opCtx->recoveryUnit()) {
        opCtx->setRecoveryUnit(std::make_unique<RecoveryUnitNoop>(),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }
    // The baton must be attached before attaching to a client
    if (_transportLayer) {
        _transportLayer->makeBaton(opCtx.get());
    } else {
        makeBaton(opCtx.get());
    }

    {
        stdx::lock_guard<Client> lk(*client);

        // If we have a previous operation context, it's not worth crashing the process in
        // production. However, we do want to prevent it from doing more work and complain loudly.
        auto lastOpCtx = client->getOperationContext();
        if (lastOpCtx) {
            killOperation(lk, lastOpCtx, ErrorCodes::Error(4946800));
            tasserted(
                4946801,
                "Client has attempted to create a new OperationContext, but it already has one");
        }

        client->_setOperationContext(opCtx.get());
    }

    {
        stdx::lock_guard lk(_mutex);
        _clientByOperationId.emplace(opCtx->getOpID(), client);
    }

    return UniqueOperationContext(opCtx.release());
};

void ServiceContext::OperationContextDeleter::operator()(OperationContext* opCtx) const {
    auto client = opCtx->getClient();
    invariant(client);

    auto service = client->getServiceContext();
    invariant(service);

    service->_delistOperation(opCtx);
    opCtx->getBaton()->detach();

    opCtx->onDestroy();
    delete opCtx;
}

LockedClient ServiceContext::getLockedClient(OperationId id) {
    stdx::lock_guard lk(_mutex);
    auto it = _clientByOperationId.find(id);
    if (it == _clientByOperationId.end()) {
        return {};
    }

    return LockedClient(it->second);
}

ServiceContext::LockedClientsCursor::LockedClientsCursor(ServiceContext* service)
    : _lock(service->_mutex), _curr(service->_clients.cbegin()), _end(service->_clients.cend()) {}

Client* ServiceContext::LockedClientsCursor::next() {
    if (_curr == _end)
        return nullptr;
    Client* result = *_curr;
    ++_curr;
    return result;
}

void ServiceContext::setKillAllOperations(const std::set<std::string>& excludedClients) {
    stdx::lock_guard<Latch> clientLock(_mutex);

    // Ensure that all newly created operation contexts will immediately be in the interrupted state
    _globalKill.store(true);
    auto opsKilled = 0;

    // Interrupt all active operations
    for (auto&& client : _clients) {
        stdx::lock_guard<Client> lk(*client);

        // Do not kill operations from the excluded clients.
        if (excludedClients.find(client->desc()) != excludedClients.end()) {
            continue;
        }

        auto opCtxToKill = client->getOperationContext();
        if (opCtxToKill) {
            killOperation(lk, opCtxToKill, ErrorCodes::InterruptedAtShutdown);
            opsKilled++;
        }
    }

    // Shared by mongos and mongod shutdown code paths
    LOGV2(4695300, "Interrupted all currently running operations", "opsKilled"_attr = opsKilled);

    // Notify any listeners who need to reach to the server shutting down
    for (const auto listener : _killOpListeners) {
        try {
            listener->interruptAll();
        } catch (...) {
            std::terminate();
        }
    }
}

void ServiceContext::killOperation(WithLock, OperationContext* opCtx, ErrorCodes::Error killCode) {
    opCtx->markKilled(killCode);

    for (const auto listener : _killOpListeners) {
        try {
            listener->interrupt(opCtx->getOpID());
        } catch (...) {
            std::terminate();
        }
    }
}

void ServiceContext::_delistOperation(OperationContext* opCtx) noexcept {
    // Removing `opCtx` from `_clientByOperationId` must always precede removing the `opCtx` from
    // its client to prevent situations that another thread could use the service context to get a
    // hold of an `opCtx` that has been removed from its client.
    {
        stdx::lock_guard lk(_mutex);
        if (_clientByOperationId.erase(opCtx->getOpID()) != 1) {
            // Another thread has already delisted this `opCtx`.
            return;
        }
    }

    auto client = opCtx->getClient();
    stdx::lock_guard clientLock(*client);
    // Reaching here implies this call was able to remove the `opCtx` from ServiceContext.

    // Assigning a new opCtx to the client must never precede the destruction of any existing opCtx
    // that references the client.
    invariant(client->getOperationContext() == opCtx);
    client->_setOperationContext({});

    if (client->session()) {
        _numCurrentOps.subtractAndFetch(1);
    }

    opCtx->releaseOperationKey();
}

void ServiceContext::killAndDelistOperation(OperationContext* opCtx,
                                            ErrorCodes::Error killCode) noexcept {

    auto client = opCtx->getClient();
    invariant(client);

    auto service = client->getServiceContext();
    invariant(service == this);

    _delistOperation(opCtx);

    stdx::lock_guard clientLock(*client);
    killOperation(clientLock, opCtx, killCode);
}

void ServiceContext::unsetKillAllOperations() {
    _globalKill.store(false);
}

void ServiceContext::registerKillOpListener(KillOpListenerInterface* listener) {
    stdx::lock_guard<Latch> clientLock(_mutex);
    _killOpListeners.push_back(listener);
}

void ServiceContext::waitForStartupComplete() {
    stdx::unique_lock<Latch> lk(_mutex);
    _startupCompleteCondVar.wait(lk, [this] { return _startupComplete; });
}

void ServiceContext::notifyStartupComplete() {
    stdx::unique_lock<Latch> lk(_mutex);
    _startupComplete = true;
    lk.unlock();
    _startupCompleteCondVar.notify_all();
}

int ServiceContext::getActiveClientOperations() {
    return _numCurrentOps.load();
}

ServiceContext::UniqueServiceContext ServiceContext::make() {
    auto service = std::make_unique<ServiceContext>();
    service->onCreate();
    return UniqueServiceContext{service.release()};
}

void ServiceContext::ServiceContextDeleter::operator()(ServiceContext* service) const {
    service->onDestroy();
    delete service;
}

BatonHandle ServiceContext::makeBaton(OperationContext* opCtx) const {
    invariant(!opCtx->getBaton());

    auto baton = std::make_shared<DefaultBaton>(opCtx);
    opCtx->setBaton(baton);

    return baton;
}

}  // namespace mongo
