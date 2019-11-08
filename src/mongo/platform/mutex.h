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

#include <type_traits>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/lock_free.h"
#include "mongo/platform/source_location.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {
namespace latch_detail {

using Level = hierarchical_acquisition_detail::Level;

static constexpr auto kAnonymousName = "AnonymousLatch"_sd;

class Identity {
public:
    Identity(StringData name) : Identity(boost::none, name, boost::none) {}

    Identity(SourceLocationHolder sourceLocation, StringData name)
        : Identity(sourceLocation, name, boost::none) {}

    Identity(StringData name, Level level) : Identity(boost::none, name, level) {}

    Identity(boost::optional<SourceLocationHolder> sourceLocation,
             StringData name,
             boost::optional<Level> level)
        : _sourceLocation(sourceLocation),
          _name(name.empty() ? kAnonymousName.toString() : name.toString()),
          _level(level),
          _id(_nextId()) {}

    const auto& level() const {
        return _level;
    }

    const auto& sourceLocation() const {
        return _sourceLocation;
    }

    StringData name() const {
        return _name;
    }

    const auto& id() const {
        return _id;
    }

private:
    static int64_t _nextId() {
        static int64_t nextId = 0;
        return nextId++;
    }

    boost::optional<SourceLocationHolder> _sourceLocation;
    std::string _name;
    boost::optional<Level> _level;
    int64_t _id;
};

class CatalogEntry {
public:
    CatalogEntry(const Identity& id_) : id(id_) {}

    const Identity id;

    AtomicWord<int> contendedCount{0};
    AtomicWord<int> acquireCount{0};
    AtomicWord<int> releaseCount{0};
};

class Catalog : public LockFreeList<CatalogEntry> {
public:
    static auto& get() {
        static Catalog gCatalog;
        return gCatalog;
    }
};

class CatalogRegistration {
public:
    CatalogRegistration(const Identity& id)
        : _entry(id), _index{latch_detail::Catalog::get().add(&_entry)} {}

    CatalogEntry* entry() {
        return &_entry;
    }

private:
    CatalogEntry _entry;
    size_t _index;
};

template <typename T>
auto registerLatch(T&&,
                   SourceLocationHolder loc,
                   StringData name,
                   boost::optional<Level> level = boost::none) {
    static auto reg = CatalogRegistration(Identity(loc, name, level));
    return reg.entry();
}

inline auto defaultCatalogEntry() {
    return registerLatch([] {}, MONGO_SOURCE_LOCATION(), kAnonymousName);
}
}  // namespace latch_detail

class Latch {
public:
    virtual ~Latch() = default;

    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual bool try_lock() = 0;

    virtual StringData getName() const {
        return latch_detail::kAnonymousName;
    }
};

class Mutex : public Latch {
public:
    class LockListener;

    void lock() override;
    void unlock() override;
    bool try_lock() override;
    StringData getName() const override;

    Mutex() : Mutex(latch_detail::defaultCatalogEntry()) {}
    Mutex(latch_detail::CatalogEntry* entry) : _entry(entry) {}

    /**
     * This function adds a LockListener subclass to the triggers for certain actions.
     *
     * LockListeners can only be added and not removed. If you wish to deactivate a LockListeners
     * subclass, please provide the switch on that subclass to noop its functions. It is only safe
     * to add a LockListener during a MONGO_INITIALIZER.
     */
    static void addLockListener(LockListener* listener);

private:
    static auto& _getListenerState() noexcept {
        struct State {
            std::vector<LockListener*> list;
        };

        // Note that state should no longer be mutated after init-time (ala MONGO_INITIALIZERS). If
        // this changes, than this state needs to be synchronized.
        static State state;
        return state;
    }

    void _onContendedLock() noexcept;
    void _onQuickLock() noexcept;
    void _onSlowLock() noexcept;
    void _onUnlock() noexcept;

    latch_detail::CatalogEntry* const _entry;
    stdx::mutex _mutex;  // NOLINT
};

/**
 * A set of actions to happen upon notable events on a Lockable-conceptualized type
 */
class Mutex::LockListener {
    friend class Mutex;

public:
    using Identity = latch_detail::Identity;

    virtual ~LockListener() = default;

    /**
     * Action to do when a lock cannot be immediately acquired
     */
    virtual void onContendedLock(const Identity& id) = 0;

    /**
     * Action to do when a lock was acquired without blocking
     */
    virtual void onQuickLock(const Identity& id) = 0;

    /**
     * Action to do when a lock was acquired after blocking
     */
    virtual void onSlowLock(const Identity& id) = 0;

    /**
     * Action to do when a lock is unlocked
     */
    virtual void onUnlock(const Identity& id) = 0;
};

}  // namespace mongo

/**
 * Define a mongo::Mutex with all arguments passed through to the ctor
 */
#define MONGO_MAKE_LATCH(name)                           \
    ::mongo::Mutex(::mongo::latch_detail::registerLatch( \
        [] {}, MONGO_SOURCE_LOCATION_NO_FUNC(), ::mongo::StringData(name)));
