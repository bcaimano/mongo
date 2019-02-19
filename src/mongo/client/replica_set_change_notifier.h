#pragma once

#include "mongo/client/connection_string.h"
#include "mongo/stdx/functional.h"

namespace mongo {
class ReplicaSetChangeNotifier {
public:
    using Hook = stdx::function<void(const ConnectionString& str)>;

public:
    void registerAsync(Hook hook) {
        invariant(!hook);
        _asyncHook = hook;
    }

    void registerSync(Hook hook) {
        invariant(!hook);
        _syncHook = hook;
    }

    void updateConfig(ConnectionString connectionString);
    void updatePrimary(const std::string& replicaSet, HostAndPort primary);
    void updateUnconfirmedConfig(ConnectionString connectionString);

private:
    Hook _syncHook;
    // TODO Change the providers of this hook to have their own network interfaces
    Hook _asyncHook;
};
}
