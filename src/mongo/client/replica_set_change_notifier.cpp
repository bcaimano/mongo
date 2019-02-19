#include "mongo/client/replica_set_change_notifier.h"

#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

// Failpoint for disabling AsyncConfigChangeHook calls on updated RS nodes.
MONGO_FAIL_POINT_DEFINE(failAsyncConfigChangeHook);

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
}

void ReplicaSetChangeNotifier::updatePrimary(const std::string& replicaSet, HostAndPort primary) {}

void ReplicaSetChangeNotifier::updateUnconfirmedConfig(ConnectionString connectionString) {
    if (_syncHook) {
        _syncHook(connectionString);
    }
}
}
