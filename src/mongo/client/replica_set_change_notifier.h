#pragma once

#include "mongo/client/connection_string.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

class ReplicaSetChangeListener {
public:
    virtual ~ReplicaSetChangeListener() = default;

    virtual void handleConfig(const ConnectionString& str) = 0;
    virtual void handlePrimary(const std::string& replicaSet, const HostAndPort& host) = 0;
};

class ReplicaSetChangeNotifier {
public:
    using Hook = stdx::function<void(const ConnectionString& str)>;
    using Listener = ReplicaSetChangeListener;

public:
    void registerAsync(Hook hook) {
        invariant(!hook);
        _asyncHook = hook;
    }

    void registerSync(Hook hook) {
        invariant(!hook);
        _syncHook = hook;
    }

    // By this point, the Listener should be fully constructed and initialized. I don't care how
    void addListener(Listener* listener) {
        _listeners.insert(listener);
    }

    void removeListener(Listener* listener) {
        _listeners.erase(listener);
    }

    void updateConfig(ConnectionString connectionString);
    void updatePrimary(const std::string& replicaSet, HostAndPort primary);
    void updateUnconfirmedConfig(ConnectionString connectionString);

private:
    Hook _syncHook;
    // TODO Change the providers of this hook to have their own network interfaces
    Hook _asyncHook;

    stdx::unordered_set<Listener*> _listeners;
};

}
