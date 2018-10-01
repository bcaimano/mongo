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
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"


namespace mongo {
namespace {
namespace moe = mongo::optionenvironment;

constexpr char kTotalOperations[] = "totalOperations";
constexpr char kAddEgressInterface[] = "addEgressInterface";
constexpr char kEgressInterfaces[] = "egressInterfaces";

Status addWorkloadGenerationOptions(moe::OptionSection* options) {
    moe::OptionSection workGenOptions("Workload Generation options");

    workGenOptions.addOptionChaining(
        kTotalOperations, kTotalOperations, moe::UnsignedLongLong, "Total operations to queue");
    workGenOptions.addOptionChaining(
        kEgressInterfaces,
        kAddEgressInterface,
        moe::StringVector,
        "Add network interface to use for egress connections");

    Status ret = options->addSection(workGenOptions);
    if (!ret.isOK()) {
        error() << "Failed to add workload generation option section: " << ret.toString();
        return ret;
    }

    return Status::OK();
}

unsigned long long totalOperations = 32768ull;
std::vector<std::string> egressInterfaces = {"127.0.0.2"};
Status storeWorkloadGenerationOptions(const moe::Environment& params) {

    if (params.count(kTotalOperations))
        totalOperations = params[kTotalOperations].as<unsigned long long>();

    if (params.count(kEgressInterfaces))
        egressInterfaces = params[kEgressInterfaces].as<std::vector<std::string>>();

    return Status::OK();
}

MONGO_MODULE_STARTUP_OPTIONS_REGISTER(WorkloadGenerationOptions)(InitializerContext* /*unused*/) {
    return addWorkloadGenerationOptions(&moe::startupOptions);
}

MONGO_STARTUP_OPTIONS_STORE(WorkloadGenerationOptions)(InitializerContext* /*unused*/) {
    return storeWorkloadGenerationOptions(moe::startupOptionsParsed);
}

}  // namespace

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

    setTestCommandsEnabled(true);

    transport::TransportLayerASIO::Options tlOpts;
    tlOpts.mode = transport::TransportLayerASIO::Options::kEgress | transport::TransportLayerASIO::Options::kIngress;
    tlOpts.ipList = egressInterfaces;

    auto tl = std::make_unique<transport::TransportLayerASIO>(tlOpts, nullptr);
    uassertStatusOK(tl->setup());
    uassertStatusOK(tl->start());
    svc->setTransportLayer(std::move(tl));

    struct Latch {
        ~Latch() {
            promise.emplaceValue();
        }

        explicit Latch(Promise<void> p) : promise(std::move(p)) {}

        Promise<void> promise;
    };

    struct Thread {
        stdx::thread thread;
        std::shared_ptr<Latch> latch;
        AtomicWord<long> maxCount;
        size_t nConns;
        size_t id{0};
    };

    constexpr size_t nThreads = 8;
    std::array<Thread, nThreads> threads;

    size_t n = 0;
    for (auto& thread : threads) {
        thread.id = n++;
        thread.nConns = totalOperations / nThreads;
        thread.thread = stdx::thread([&svc, &thread] {
            auto pf = makePromiseFuture<void>();
            thread.latch = std::make_shared<Latch>(std::move(pf.promise));
            auto future = std::move(pf.future);

            ConnectionPool::Options cpOpts;
            cpOpts.refreshRequirement = Minutes(5);
            cpOpts.refreshTimeout = Minutes(5);
            cpOpts.maxConnecting = 1024ul;

            auto niName = std::string(str::stream() << "interface" << thread.id);
            NetworkInterfaceTL ni(niName, cpOpts, svc, nullptr, nullptr);
            ni.startup();

            auto cs = unittest::getFixtureConnectionString();

            RemoteCommandRequest rcr(cs.getServers().front(),
                                     "admin",
                                     BSON("sleep" << 1 << "lock"
                                                  << "none"
                                                  << "secs"
                                                  << 6000),
                                     nullptr);
            //    RemoteCommandRequest rcr(cs.getServers().front(), "admin", BSON("ping" << 1),
            //    nullptr);

            for (size_t i = 0; i < thread.nConns; i++) {
                ni.NetworkInterface::startCommand(makeCallbackHandle(), rcr)
                    .getAsync([localLatch = thread.latch, &niName, &thread](StatusWith<TaskExecutor::ResponseStatus> rs) mutable {
                        uassertStatusOK(rs);

                        thread.maxCount.store(std::max(localLatch.use_count(), thread.maxCount.load()));
                        //log() << niName << " use count at: " << latch.use_count();
                        localLatch.reset();
                    });
            }

            thread.latch.reset();
            log() << "All commands started. Waiting for latch.";

            future.get();

            ASSERT_EQ(ni.getCounters().failed, 0ul);
            ASSERT_EQ(ni.getCounters().timedOut, 0ul);

            ni.shutdown();
        });
    }

    for (auto& thread : threads) {
        thread.thread.join();
        ASSERT_EQ(thread.latch.use_count(), 0l);
        log() << "Thread " << thread.id << " max use: " << thread.maxCount.load();
    }

    svc->getTransportLayer()->shutdown();
}

}  // namespace executor
}  // namespace mongo
