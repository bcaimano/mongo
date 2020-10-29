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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/util/background.h"

#include <functional>

#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

namespace mongo {

// both the BackgroundJob and the internal thread point to JobStatus
struct BackgroundJob::JobStatus {
    JobStatus() : state(NotStarted) {}

    Mutex mutex = MONGO_MAKE_LATCH("JobStatus::mutex");
    stdx::condition_variable done;
    State state;
};

BackgroundJob::BackgroundJob(bool selfDelete) : _selfDelete(selfDelete), _status(new JobStatus) {}

BackgroundJob::~BackgroundJob() {}

void BackgroundJob::jobBody() {
    const std::string threadName = name();
    if (!threadName.empty()) {
        setThreadName(threadName);
    }

    LOGV2_DEBUG(23098,
                1,
                "BackgroundJob starting: {threadName}",
                "BackgroundJob starting",
                "threadName"_attr = threadName);

    run();

    // We must cache this value so that we can use it after we leave the following scope.
    const bool selfDelete = _selfDelete;

    {
        // It is illegal to access any state owned by this BackgroundJob after leaving this
        // scope, with the exception of the call to 'delete this' below.
        stdx::unique_lock<Latch> l(_status->mutex);
        _status->state = Done;
        _status->done.notify_all();
    }

    if (selfDelete)
        delete this;
}

void BackgroundJob::go() {
    stdx::unique_lock<Latch> l(_status->mutex);
    massert(17234,
            str::stream() << "backgroundJob already running: " << name(),
            _status->state != Running);

    // If the job is already 'done', for instance because it was cancelled or already
    // finished, ignore additional requests to run the job.
    if (_status->state == NotStarted) {
        stdx::thread{[this] { jobBody(); }}.detach();
        _status->state = Running;
    }
}

Status BackgroundJob::cancel() {
    stdx::unique_lock<Latch> l(_status->mutex);

    if (_status->state == Running)
        return Status(ErrorCodes::IllegalOperation, "Cannot cancel a running BackgroundJob");

    if (_status->state == NotStarted) {
        _status->state = Done;
        _status->done.notify_all();
    }

    return Status::OK();
}

bool BackgroundJob::wait(unsigned msTimeOut) {
    verify(!_selfDelete);  // you cannot call wait on a self-deleting job
    const auto deadline = Date_t::now() + Milliseconds(msTimeOut);
    stdx::unique_lock<Latch> l(_status->mutex);
    while (_status->state != Done) {
        if (msTimeOut) {
            if (stdx::cv_status::timeout ==
                _status->done.wait_until(l, deadline.toSystemTimePoint()))
                return false;
        } else {
            _status->done.wait(l);
        }
    }
    return true;
}

BackgroundJob::State BackgroundJob::getState() const {
    stdx::unique_lock<Latch> l(_status->mutex);
    return _status->state;
}

bool BackgroundJob::running() const {
    stdx::unique_lock<Latch> l(_status->mutex);
    return _status->state == Running;
}

}  // namespace mongo
