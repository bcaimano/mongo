/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/util/latch_analyzer.h"

#include "mongo/util/hierarchical_acquisition.h"

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(enableLatchAnalysis);

bool shouldAnalyzeLatches() {
    return enableLatchAnalysis.shouldFail();
}

auto kLatchAnalysisName = "latchAnalysis"_sd;

const auto getLatchAnalyzer = ServiceContext::declareDecoration<LatchAnalyzer>();

using LatchSet = stdx::unordered_set<const latch_detail::Identity*>;
const auto getLatchSet = Client::declareDecoration<LatchSet>();

struct LatchSetState {
    HierarchicalAcquisitionSet levelsHeld;
};

const auto getLatchSetState = Client::declareDecoration<LatchSetState>();

/**
 * LockListener sub-class to implement updating set in LatchSetState
 */
class LockListener : public Mutex::LockListener {
public:
    void onContendedLock(const Identity& id) override {
        if (auto client = Client::getCurrent()) {
            LatchAnalyzer::get(client).onContention(id);
        }
    }

    void onQuickLock(const Identity& id) override {
        if (auto client = Client::getCurrent()) {
            LatchAnalyzer::get(client).onAcquire(id);
        }
    }

    void onSlowLock(const Identity& id) override {
        if (auto client = Client::getCurrent()) {
            LatchAnalyzer::get(client).onAcquire(id);
        }
    }

    void onUnlock(const Identity& id) override {
        if (auto client = Client::getCurrent()) {
            LatchAnalyzer::get(client).onRelease(id);
        }
    }
};

MONGO_INITIALIZER(LatchAnalysis)(InitializerContext* context) {

    // Intentionally leaked, people use Latches in detached threads
    static auto& listener = *new LockListener;
    Mutex::addLockListener(&listener);

    return Status::OK();
}

class LatchAnalysisSection final : public ServerStatusSection {
public:
    LatchAnalysisSection() : ServerStatusSection(kLatchAnalysisName.toString()) {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder analysis;
        LatchAnalyzer::get(opCtx->getClient()).appendToBSON(analysis);
        return analysis.obj();
    };
} gLatchAnalysisSection;

}  // namespace

LatchAnalyzer& LatchAnalyzer::get(ServiceContext* serviceContext) {
    return getLatchAnalyzer(serviceContext);
}

LatchAnalyzer& LatchAnalyzer::get(Client* client) {
    return get(client->getServiceContext());
}

void LatchAnalyzer::onContention(const latch_detail::Identity&) {
    // Nothing at the moment
}

void LatchAnalyzer::onAcquire(const latch_detail::Identity& id) {
    auto client = Client::getCurrent();
    if (!client) {
        return;
    }

    if (shouldAnalyzeLatches()) {
        auto& latchSet = getLatchSet(client);

        stdx::lock_guard lk(_mutex);
        for (auto otherId : latchSet) {
            auto& stat = _hierarchies[id.id()][otherId->id()];
            stat.id = otherId;
            ++stat.acquiredAfter;
        }

        latchSet.insert(&id);
    }

    if (!id.level()) {
        return;
    }

    auto level = *id.level();
    auto& handle = getLatchSetState(client);
    auto result = handle.levelsHeld.add(level);
    if (result != HierarchicalAcquisitionSet::AddResult::kValidWasAbsent) {
        // TODO: SERVER-44570 Create a non process-fatal variant of invariant()
        fassert(31360,
                Status(ErrorCodes::HierarchicalAcquisitionLevelViolation,
                       str::stream() << "Theoretical deadlock alert - " << toString(result)
                                     << " latch acquisition at " << id.sourceLocation()->toString()
                                     << " on " << id.name()));
    }
}

void LatchAnalyzer::onRelease(const latch_detail::Identity& id) {
    auto client = Client::getCurrent();
    if (!client) {
        return;
    }

    if (shouldAnalyzeLatches()) {
        auto& latchSet = getLatchSet(client);
        latchSet.erase(&id);

        stdx::lock_guard lk(_mutex);
        for (auto otherId : latchSet) {
            auto& stat = _hierarchies[id.id()][otherId->id()];
            stat.id = otherId;
            ++stat.releasedBefore;
        }
    }

    if (!id.level()) {
        return;
    }

    auto level = *id.level();
    auto& handle = getLatchSetState(client);
    auto result = handle.levelsHeld.remove(level);
    if (result != HierarchicalAcquisitionSet::RemoveResult::kValidWasPresent) {
        // TODO: SERVER-44570 Create a non process-fatal variant of invariant()
        fassert(31361,
                Status(ErrorCodes::HierarchicalAcquisitionLevelViolation,
                       str::stream() << "Theoretical deadlock alert - " << toString(result)
                                     << " latch release at " << id.sourceLocation()->toString()
                                     << " on " << id.name()));
    }
}

void LatchAnalyzer::appendToBSON(mongo::BSONObjBuilder& result) const {
    for (auto iter = latch_detail::Catalog::get().iter(); iter.more();) {
        auto ptr = iter.next();
        if (!ptr) {
            continue;
        }

        auto& id = ptr->id;

        auto afterName = id.name();
        BSONObjBuilder latchObj = result.subobjStart(afterName);
        latchObj.append("acquired", ptr->acquireCount.loadRelaxed());
        latchObj.append("released", ptr->releaseCount.loadRelaxed());
        latchObj.append("contended", ptr->contendedCount.loadRelaxed());

        if (!shouldAnalyzeLatches()) {
            continue;
        }

        stdx::lock_guard lk(_mutex);
        auto it = _hierarchies.find(id.id());
        if (it == _hierarchies.end()) {
            continue;
        }

        auto& latchHierarchy = it->second;
        if (latchHierarchy.empty()) {
            continue;
        }

        {
            BSONObjBuilder acquiredAfterObj = latchObj.subobjStart("acquiredAfter");
            for (auto& [otherId, stat] : latchHierarchy) {
                auto count = stat.acquiredAfter;
                if (count == 0) {
                    continue;
                }
                acquiredAfterObj.append(stat.id->name(), count);
            }
        }

        {
            BSONObjBuilder releasedBeforeObj = latchObj.subobjStart("releasedBefore");
            for (auto& [otherId, stat] : latchHierarchy) {
                auto count = stat.releasedBefore;
                if (count == 0) {
                    continue;
                }
                releasedBeforeObj.append(stat.id->name(), count);
            }
        }
    }
}

void LatchAnalyzer::dump() {
    if (!shouldAnalyzeLatches()) {
        return;
    }

    BSONObjBuilder bob(1024 * 1024);
    {
        BSONObjBuilder analysis = bob.subobjStart("latchAnalysis");
        appendToBSON(analysis);
    }

    auto obj = bob.done();
    log().setIsTruncatable(false) << "=====LATCHES=====\n"
                                  << obj.jsonString() << "\n===END LATCHES===";
}

}  // namespace mongo
