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

#include "mongo/platform/atomic_word.h"

namespace mongo {

template <typename T, size_t kBlockSize = 4096>
class LockFreeList {
    // Acquire a page at a time in deque segments.
    //
    // This may be greater than the total number of instantiations in the system, and that's
    // just fine.
    static constexpr size_t kCapacityGranularity = kBlockSize / sizeof(T*);

    using DataType = std::deque<T*>;
    using IndexType = AtomicWord<size_t>;

public:
    class Iterator {
    public:
        explicit Iterator(const LockFreeList& list) : _list(list) {}

        auto next() {
            return _list._data[_index++];
        }

        bool more() const {
            return _index < _list._readEnd.load();
        }

    private:
        const LockFreeList& _list;
        size_t _index = 0;
    };

    size_t add(T* ptr) {
        // Grab our write index from the counter
        auto index = _getFreshIndex();
        _data[index] = ptr;

        size_t prev = index - 1;
        while (!_readEnd.compareAndSwap(&prev, index)) {
            // Loop until we've successfully incremented up to our index
        }
        return index;
    }

    T* get(size_t index) const {
        if (index >= _readEnd.load()) {
            // If index is past our synchronized end on the deque, then indexing it will be UB.
            return nullptr;
        }

        return _data[index];
    }

    auto iter() const {
        return Iterator(*this);
    }

    size_t size() const {
        return _readEnd.load();
    }

private:
    size_t _getFreshIndex() {
        // This function relies on the idea that indexes increase montonically and are never
        // skipped. If somehow the index == _capacity never occurs, then the while loop below will
        // never resolve.

        const auto index = _nextWriteIndex.fetchAndAdd(1);

        auto currentCapacity = _capacity.load();
        if (index == currentCapacity) {
            // If we are exactly at capacity, it's our responsibility to expand the data deque
            auto newCapacity = currentCapacity + kCapacityGranularity;
            _data.resize(newCapacity, nullptr);
            _capacity.store(newCapacity);
            return index;
        }

        while (currentCapacity < index) {
            // If the capacity is less than our index, then we need to loop until the responsible
            // thread increases the capacity
            currentCapacity = _capacity.load();
        }

        return index;
    }

    IndexType _nextWriteIndex{0};
    IndexType _readEnd{0};

    DataType _data;
    IndexType _capacity{0};
};

}  // namespace mongo
