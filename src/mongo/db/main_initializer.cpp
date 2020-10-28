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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/db/main_initializer.h"

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/logv2/log.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/thread_context.h"
#include "mongo/util/thread_safety_context.h"
#include "mongo/util/time_support.h"

namespace mongo {

MONGO_INITIALIZER_GENERAL(InitMainThreadContext, ("ServerGlobalParams"), MONGO_NO_DEPENDENTS)
(InitializerContext* context) {
    // Initialize our first thread context after we make our global params but before we make
    // ServerParameters.
    ThreadContext::init();

    return Status::OK();
}

void MainInitializer::begin() try {
    ThreadSafetyContext::getThreadSafetyContext()->forbidMultiThreading();

    setupSignalHandlers();

    srand(static_cast<unsigned>(curTimeMicros64()));

    uassertStatusOK(mongo::runGlobalInitializers(_args));

    ErrorExtraInfo::invariantHaveAllParsers();
} catch (const DBException& e) {
    LOGV2_FATAL_OPTIONS(
        20574,
        logv2::LogOptions(logv2::LogComponent::kControl, logv2::FatalMode::kContinue),
        "Error during global initialization: {error}",
        "Error during global initialization",
        "error"_attr = e);
    throw;
}

void MainInitializer::finish() {
    // There is no single-threaded guarantee beyond this point.
    ThreadSafetyContext::getThreadSafetyContext()->allowMultiThreading();

    // Per SERVER-7434, startSignalProcessingThread must run after any forks (i.e.
    // initializeServerGlobalState) and before the creation of any other threads
    startSignalProcessingThread();

    cmdline_utils::censorArgvArray(_argc, _argv);
}

}  // namespace mongo
