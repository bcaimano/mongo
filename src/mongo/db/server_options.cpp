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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/parameters_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/util/thread_context.h"

namespace mongo {

namespace {
/**
 * This struct represents global configuration data for the server.  These options get set from
 * the command line and are used inline in the code.  Note that much shared code uses this
 * struct, which is why it is here in its own file rather than in the same file as the code that
 * sets it via the command line, which would pull in more dependencies.
 */
auto getServerParams = ThreadContext::declareDecoration<std::shared_ptr<ServerGlobalParams>>();

auto threadConstructorAction = ThreadContext::ConstructorActionRegisterer(
    "ServerGlobalParams", [](ThreadContext* threadContext) {
        auto parentThreadContext = threadContext->getParent();
        if (!parentThreadContext) {
            // The main thread gets a new ServerGlobalParams. It probably shouldn't, but I don't
            // feel like tracking down the spots where it is used before init.
            getServerParams(threadContext) = std::make_shared<ServerGlobalParams>();
            return;
        }

        // Each thread starts with its parent's params
        auto serverParams = getServerParams(parentThreadContext.get());
        getServerParams(threadContext) = std::move(serverParams);
    });

FeatureCompatibility featureCompatibility;

}  // namespace

ServerGlobalParams& getStaticServerParams() {
    auto threadContext = ThreadContext::get();
    auto serverParams = getServerParams(threadContext.get());
    invariant(serverParams);
    return *serverParams;
}

const FeatureCompatibility& getFeatureCompatibility() {
    return featureCompatibility;
}

void setFeatureCompatibility(FeatureCompatibility::Version version) {
    featureCompatibility.setVersion(version);
}

AtomicWord<bool> gBeQuiet;

bool shouldBeQuiet() {
    return gBeQuiet.loadRelaxed();
}

void setBeQuiet(bool beQuiet) {
    gBeQuiet.store(beQuiet);
}

std::string ServerGlobalParams::getPortSettingHelpText() {
    return str::stream() << "Specify port number - " << getStaticServerParams().port
                         << " by default";
}

}  // namespace mongo
