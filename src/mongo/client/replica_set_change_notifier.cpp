#include "mongo/client/replica_set_change_notifier.h"

#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

// Failpoint for disabling AsyncConfigChangeHook calls on updated RS nodes.
MONGO_FAIL_POINT_DEFINE(failAsyncConfigChangeHook);

void ReplicaSetChangeNotifier::addListener(Listener* listener) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    _listeners.insert(listener);
    for (auto && [ replSet, data ] : _lastChange) {
        listener->handleConfig(data->connStr);
        listener->handlePrimary(data->connStr.getSetName(), data->primary);
    }
}

void ReplicaSetChangeNotifier::updateConfig(ConnectionString connectionString) {
    if (_syncHook) {
        _syncHook(connectionString);
    }

    if (_asyncHook && !MONGO_FAIL_POINT(failAsyncConfigChangeHook)) {
        // call from a separate thread to avoid blocking and holding lock while potentially
        // going over the network
        stdx::thread bg(_asyncHook, connectionString);
        bg.detach();
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto& data = _lastChange[connectionString.getSetName()];
    if (!data) {
        data = std::make_unique<Data>();
    }
    data->connStr = connectionString;

    for (auto listener : _listeners) {
        listener->handleConfig(data->connStr);
    }
}

void ReplicaSetChangeNotifier::updatePrimary(const std::string& replicaSet, HostAndPort primary) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto& data = _lastChange[replicaSet];
    if (!data) {
        data = std::make_unique<Data>();
    }
    data->primary = primary;

    for (auto listener : _listeners) {
        listener->handlePrimary(replicaSet, data->primary);
    }
}

void ReplicaSetChangeNotifier::updateUnconfirmedConfig(ConnectionString connectionString) {
    if (_syncHook) {
        _syncHook(connectionString);
    }
}
}
