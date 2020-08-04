/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_executor_utils.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/signal_handlers.h"

namespace mongo {

constexpr auto kMessage = "Probing noexcept";

void violateNoexcept() {
    violateNoexcept(ErrorCodes::InternalError, kMessage);
}

class NoexceptTest : public unittest::Test {
public:
    void setUp() override {
        setupSignalHandlers();
        startSignalProcessingThread();
    }
};

DEATH_TEST_F(NoexceptTest, InMainThread, kMessage) {
    violateNoexcept();
}

DEATH_TEST_F(NoexceptTest, InStdxThread, kMessage) {
    stdx::thread([] { violateNoexcept(); }).join();
}

DEATH_TEST_F(NoexceptTest, InClientThread, kMessage) {
    auto barrier = unittest::Barrier(2);

    ASSERT_OK(launchServiceWorkerThread([&] {
        violateNoexcept();
        barrier.countDownAndWait();
    }));

    barrier.countDownAndWait();
}

DEATH_TEST_F(NoexceptTest, InUniqueFunction, kMessage) {
    auto fun = unique_function([] { violateNoexcept(); });
    fun();
}

DEATH_TEST_F(NoexceptTest, InGetAsyncFunction, kMessage) {
    auto [promise, future] = makePromiseFuture<void>();
    std::move(future).getAsync([](Status) { violateNoexcept(); });

    promise.emplaceValue();
}

DEATH_TEST_F(NoexceptTest, InThreadPool, kMessage) {
    auto barrier = unittest::Barrier(2);

    auto pool = ThreadPool(ThreadPool::Options{});
    pool.startup();

    pool.schedule([&](Status) {
        violateNoexcept();
        barrier.countDownAndWait();
    });

    barrier.countDownAndWait();
}

}  // namespace mongo
