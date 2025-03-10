/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth

#include "mongo/db/process_health/fault_manager.h"

#include "mongo/logv2/log.h"

namespace mongo {

namespace process_health {

namespace {

const auto sFaultManager = ServiceContext::declareDecoration<std::unique_ptr<FaultManager>>();

}  // namespace

ServiceContext::ConstructorActionRegisterer faultManagerRegisterer{
    "FaultManagerRegisterer", [](ServiceContext* svcCtx) {
        auto faultManager = std::make_unique<FaultManager>();
        FaultManager::set(svcCtx, std::move(faultManager));
    }};


FaultManager* FaultManager::get(ServiceContext* svcCtx) {
    return sFaultManager(svcCtx).get();
}

void FaultManager::set(ServiceContext* svcCtx, std::unique_ptr<FaultManager> newFaultManager) {
    invariant(newFaultManager);
    auto& faultManager = sFaultManager(svcCtx);
    faultManager = std::move(newFaultManager);
}

FaultManager::~FaultManager() {}

FaultState FaultManager::getFaultState() const {
    return FaultState::kOk;
}

boost::optional<FaultConstPtr> FaultManager::activeFault() const {
    return {};
}

void FaultManager::healthCheck() {}

}  // namespace process_health
}  // namespace mongo
