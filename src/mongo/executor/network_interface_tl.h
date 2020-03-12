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

#pragma once

#include <deque>

#include "mongo/client/async_client.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/hedging_metrics.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/strong_weak_finish_line.h"

namespace mongo {
namespace executor {

class NetworkInterfaceTL : public NetworkInterface {
    static constexpr int kDiagnosticLogLevel = 2;

public:
    NetworkInterfaceTL(std::string instanceName,
                       ConnectionPool::Options connPoolOpts,
                       ServiceContext* ctx,
                       std::unique_ptr<NetworkConnectionHook> onConnectHook,
                       std::unique_ptr<rpc::EgressMetadataHook> metadataHook);
    ~NetworkInterfaceTL();

    constexpr static Milliseconds kCancelCommandTimeout{1000};

    std::string getDiagnosticString() override;
    void appendConnectionStats(ConnectionPoolStats* stats) const override;
    std::string getHostName() override;
    Counters getCounters() const override;

    void startup() override;
    void shutdown() override;
    bool inShutdown() const override;
    void waitForWork() override;
    void waitForWorkUntil(Date_t when) override;
    void signalWorkAvailable() override;
    Date_t now() override;
    Status startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                        RemoteCommandRequestOnAny& request,
                        RemoteCommandCompletionFn&& onFinish,
                        const BatonHandle& baton) override;
    Status startExhaustCommand(const TaskExecutor::CallbackHandle& cbHandle,
                               RemoteCommandRequestOnAny& request,
                               RemoteCommandOnReplyFn&& onReply,
                               const BatonHandle& baton) override;

    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                       const BatonHandle& baton) override;
    Status setAlarm(const TaskExecutor::CallbackHandle& cbHandle,
                    Date_t when,
                    unique_function<void(Status)> action) override;

    Status schedule(unique_function<void(Status)> action) override;

    void cancelAlarm(const TaskExecutor::CallbackHandle& cbHandle) override;

    bool onNetworkThread() override;

    void dropConnections(const HostAndPort& hostAndPort) override;

    void testEgress(const HostAndPort& hostAndPort,
                    transport::ConnectSSLMode sslMode,
                    Milliseconds timeout,
                    Status status) override;

private:
    struct RequestState;
    struct CommandStateBase;

    using ConnectionHandle = std::shared_ptr<ConnectionPool::ConnectionHandle::element_type>;
    using WeakConnectionHandle = std::weak_ptr<ConnectionPool::ConnectionHandle::element_type>;

    class RequestManager {
    public:
        RequestManager(CommandStateBase* cmdState);

        /**
         * Attempt to send a request using the given connection
         */
        void trySend(StatusWith<ConnectionPool::ConnectionHandle> swConn, size_t idx) noexcept;

        void cancelRequests() noexcept;

        /**
         * Return true if any requests have been sent out
         */
        bool hasSentRequests() const {
            return _connsAcquired.load() > 0;
        }

        bool hasSentAllRequests() const {
            return _connsAcquired.load() >= _weakConns.size();
        }

        bool markRequestResolved() {
            return _requestsResolved.addAndFetch(1) == _weakConns.size();
        }

        auto timer() const {
            return _timer.get();
        }

    private:
        CommandStateBase* _cmdState;
        std::unique_ptr<transport::ReactorTimer> _timer;
        std::vector<WeakConnectionHandle> _weakConns;

        AtomicWord<size_t> _connsResolved{0};
        AtomicWord<size_t> _connsAcquired{0};
        AtomicWord<size_t> _requestsResolved{0};
        AtomicWord<bool> _done{false};
    };

    struct CommandStateBase : public std::enable_shared_from_this<CommandStateBase> {
        enum class Events {
            kSent,
            kFinished,
            kCanceled,
        };

        CommandStateBase(NetworkInterfaceTL* interface_,
                         RemoteCommandRequestOnAny request_,
                         const TaskExecutor::CallbackHandle& cbHandle_);
        virtual ~CommandStateBase() = default;

        size_t maxConcurrentRequests() const noexcept {
            if (!requestOnAny.hedgeOptions) {
                return 1ull;
            }

            return requestOnAny.hedgeOptions->count + 1ull;
        }

        /**
         * Use the current RequestState to send out a command request.
         */
        virtual Future<RemoteCommandResponse> sendRequest(
            const std::shared_ptr<RequestState>& requestState) = 0;

        /**
         * Set a timer to fulfill the promise with a timeout error.
         */
        virtual void setTimer();

        /**
         * Fulfill the promise with the response.
         */
        virtual void fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse> response) = 0;

        auto getAnchor() noexcept {
            return shared_from_this();
        }

        /**
         * Fulfill the promise for the Command.
         *
         * This will throw/invariant if called multiple times. In an ideal world, this would do the
         * swap on CommandState::done for you and return early if it was already true. It does not
         * do so currently.
         */
        void tryFinish(Status status) noexcept;

        /**
         * Run the NetworkInterface's MetadataHook on a given request if this Command isn't already
         * finished.
         */
        void doMetadataHook(const RemoteCommandOnAnyResponse& response);

        void cancel();

        NetworkInterfaceTL* interface;

        RemoteCommandRequestOnAny requestOnAny;
        TaskExecutor::CallbackHandle cbHandle;
        Date_t deadline = kNoExpirationDate;

        ClockSource::StopWatch stopwatch;

        BatonHandle baton;

        boost::optional<UUID> operationKey;
        boost::optional<RequestManager> requestManager;

        AtomicWord<bool> done;
    };

    struct CommandState final : public CommandStateBase {
        CommandState(NetworkInterfaceTL* interface_,
                     RemoteCommandRequestOnAny request_,
                     const TaskExecutor::CallbackHandle& cbHandle_);

        // Create a new CommandState in a shared_ptr
        // Prefer this over raw construction
        static auto make(NetworkInterfaceTL* interface,
                         RemoteCommandRequestOnAny request,
                         const TaskExecutor::CallbackHandle& cbHandle);

        Future<RemoteCommandResponse> sendRequest(
            const std::shared_ptr<RequestState>& requestState) override;

        void fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse> response) override;

        Promise<RemoteCommandOnAnyResponse> promise;
    };

    struct ExhaustCommandState final : public CommandStateBase {
        ExhaustCommandState(NetworkInterfaceTL* interface_,
                            RemoteCommandRequestOnAny request_,
                            const TaskExecutor::CallbackHandle& cbHandle_,
                            RemoteCommandOnReplyFn&& onReply_);
        virtual ~ExhaustCommandState() = default;

        // Create a new ExhaustCommandState in a shared_ptr
        // Prefer this over raw construction
        static auto make(NetworkInterfaceTL* interface,
                         RemoteCommandRequestOnAny request,
                         const TaskExecutor::CallbackHandle& cbHandle,
                         RemoteCommandOnReplyFn&& onReply);

        Future<RemoteCommandResponse> sendRequest(
            const std::shared_ptr<RequestState>& requestState) override;

        void fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse> response) override;

        void continueExhaustRequest(std::shared_ptr<RequestState> requestState,
                                    StatusWith<RemoteCommandResponse> swResponse);

        Promise<void> promise;
        Promise<RemoteCommandResponse> finalResponsePromise;
        RemoteCommandOnReplyFn onReplyFn;
    };

    struct RequestState final : public std::enable_shared_from_this<RequestState> {
        RequestState(std::shared_ptr<CommandStateBase> cmdState_)
            : cmdState(std::move(cmdState_)) {}

        ~RequestState();

        /**
         * Return the client for a given connection
         */
        static AsyncDBClient* getClient(const ConnectionHandle& conn) noexcept;

        /**
         * Return the current connection to the pool and unset it locally.
         *
         * This must be called from the networking thread (i.e. the reactor).
         */
        void returnConnection(Status status) noexcept;

        /**
         * Resolve an eventual response
         */
        void resolve(Future<RemoteCommandResponse> future) noexcept;

        auto interface() noexcept {
            return cmdState->interface;
        }

        auto isHedged() const noexcept {
            return connIdForRequest;
        }

        std::shared_ptr<CommandStateBase> cmdState;

        ClockSource::StopWatch stopwatch;

        RemoteCommandRequest request;
        ConnectionHandle conn;
        size_t connIdForRequest;
    };

    struct AlarmState {
        AlarmState(Date_t when_,
                   TaskExecutor::CallbackHandle cbHandle_,
                   std::unique_ptr<transport::ReactorTimer> timer_,
                   Promise<void> promise_)
            : cbHandle(std::move(cbHandle_)),
              when(when_),
              timer(std::move(timer_)),
              promise(std::move(promise_)) {}

        TaskExecutor::CallbackHandle cbHandle;
        Date_t when;
        std::unique_ptr<transport::ReactorTimer> timer;

        AtomicWord<bool> done;
        Promise<void> promise;
    };

    void _shutdownAllAlarms();
    void _answerAlarm(Status status, std::shared_ptr<AlarmState> state);

    void _run();

    std::string _instanceName;
    ServiceContext* _svcCtx = nullptr;
    HedgingMetrics* _hedgingMetrics = nullptr;
    transport::TransportLayer* _tl = nullptr;
    // Will be created if ServiceContext is null, or if no TransportLayer was configured at startup
    std::unique_ptr<transport::TransportLayer> _ownedTransportLayer;
    transport::ReactorHandle _reactor;

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(3), "NetworkInterfaceTL::_mutex");
    ConnectionPool::Options _connPoolOpts;
    std::unique_ptr<NetworkConnectionHook> _onConnectHook;
    std::shared_ptr<ConnectionPool> _pool;

    class SynchronizedCounters;
    std::shared_ptr<SynchronizedCounters> _counters;

    std::unique_ptr<rpc::EgressMetadataHook> _metadataHook;

    // We start in kDefault, transition to kStarted after startup() is complete and enter kStopped
    // at the first call to shutdown()
    enum State : int {
        kDefault,
        kStarted,
        kStopped,
    };
    AtomicWord<State> _state;
    stdx::thread _ioThread;

    Mutex _inProgressMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "NetworkInterfaceTL::_inProgressMutex");
    stdx::unordered_map<TaskExecutor::CallbackHandle, std::weak_ptr<CommandStateBase>> _inProgress;

    bool _inProgressAlarmsInShutdown = false;
    stdx::unordered_map<TaskExecutor::CallbackHandle, std::shared_ptr<AlarmState>>
        _inProgressAlarms;

    stdx::condition_variable _workReadyCond;
    bool _isExecutorRunnable = false;
};

}  // namespace executor
}  // namespace mongo
