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

#include "boost/optional.hpp"

#include "mongo/platform/process_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

/**
 * A ThreadContext is a simple decorable that has an explicit one-to-one relationship with threads.
 */
class ThreadContext final : public Decorable<ThreadContext>, public RefCountable {
public:
    explicit ThreadContext(boost::intrusive_ptr<ThreadContext> parent)
        : _parent(std::move(parent)) {}
    virtual ~ThreadContext() = default;

    static auto make(boost::intrusive_ptr<ThreadContext> parent) {
        auto context = make_intrusive<ThreadContext>(std::move(parent));
        context->_isActive = true;
        context->_onCreate();
        return context;
    }

    static const boost::intrusive_ptr<ThreadContext>& get() {
        return _guard.instance;
    }

    static void init(boost::intrusive_ptr<ThreadContext> parent = {}) {
        invariant(!_guard.instance, "ThreadContext initialized multiple times");

        _guard.instance = make(std::move(parent));
    }

    const auto& getParent() const {
        return _parent;
    }

    const auto& threadId() const {
        return _threadId;
    }

private:
    struct MoveThenDestroyGuard {
        ~MoveThenDestroyGuard() {
            // Remove from the thread local access, then destroy.
            auto localInstance = std::exchange(instance, {});

            localInstance->_isActive = false;
            localInstance->_onDestroy();

            localInstance.reset();
        }

        boost::intrusive_ptr<ThreadContext> instance;
    };

    inline static thread_local auto _guard = MoveThenDestroyGuard{};

    const ProcessId _threadId = ProcessId::getCurrentThreadId();

    boost::intrusive_ptr<ThreadContext> _parent;
    bool _isActive = true;
};

}  // namespace mongo
