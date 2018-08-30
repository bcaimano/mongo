/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/connection_string.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace executor {

/**
 * A mock class mimicking TaskExecutor::CallbackState, does nothing.
 */
class MockCallbackState final : public TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

inline TaskExecutor::CallbackHandle makeCallbackHandle() {
    return TaskExecutor::CallbackHandle(std::make_shared<MockCallbackState>());
}

TEST(NetworkInterfaceTest, main) {
    setGlobalServiceContext(ServiceContext::make());
    auto svc = getGlobalServiceContext();

    transport::TransportLayerASIO::Options tlOpts;
    tlOpts.mode = transport::TransportLayerASIO::Options::kEgress;
    // tlOpts.ipList = { "127.0.0.1", "127.0.0.2"};
    tlOpts.ipList = {"127.0.0.1", "127.0.0.2", "127.0.0.3", "127.0.0.4", "127.0.0.5"};

    auto tl = std::make_unique<transport::TransportLayerASIO>(tlOpts, nullptr);
    uassertStatusOK(tl->start());
    svc->setTransportLayer(std::move(tl));

    struct Latch {
        ~Latch() {
            promise.emplaceValue();
        }

        explicit Latch(Promise<void> p) : promise(std::move(p)) {}

        Promise<void> promise;
    };

    constexpr size_t nThreads = 8;
    std::array<stdx::thread, 8> threads;

    size_t n = 0;
    for (auto& thread : threads) {
        thread = stdx::thread([&svc, id = n++]{
            auto pf = makePromiseFuture<void>();
            auto latch = std::make_shared<Latch>(std::move(pf.promise));
            auto future = std::move(pf.future);

            ConnectionPool::Options cpOpts;
            cpOpts.refreshRequirement = Minutes(5);
            cpOpts.refreshTimeout = Minutes(5);
            NetworkInterfaceTL ni("foo", cpOpts, svc, nullptr, nullptr);
            ni.startup();

            auto cs = unittest::getFixtureConnectionString();

            RemoteCommandRequest rcr(cs.getServers().front(), "admin", BSON("sleep" << 1 << "lock" << "none" << "secs" <<  10), nullptr);
        //    RemoteCommandRequest rcr(cs.getServers().front(), "admin", BSON("ping" << 1), nullptr);

            for (size_t i = 0; i < 100000 / nThreads; i++) {
                ni.NetworkInterface::startCommand(makeCallbackHandle(), rcr)
                    .getAsync([latch, id](StatusWith<TaskExecutor::ResponseStatus> rs) mutable {
                        if (!rs.isOK()) {
                            log() << id << " Error: " << rs.getStatus();
                        } else {
                            log() << id << " use count at: " << latch.use_count();
                        }
                        latch.reset();
                    });
            }

            latch.reset();

            future.get();

            ni.shutdown();
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    svc->getTransportLayer()->shutdown();
}

}  // namespace executor
}  // namespace mongo
