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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplicationInitialSync

#include "mongo/platform/basic.h"

#include "mongo/db/repl/database_cloner.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <set>

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {

// Failpoint which causes the initial sync function to hang before running listCollections.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeListCollections);

namespace {

using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using executor::RemoteCommandRequest;

const char* kNameFieldName = "name";
const char* kOptionsFieldName = "options";
const char* kInfoFieldName = "info";
const char* kUUIDFieldName = "uuid";

// Failpoint which causes initial sync to hang right after listCollections, but before cloning
// any colelctions in the 'database' database.
MONGO_FAIL_POINT_DEFINE(initialSyncHangAfterListCollections);

/**
 * Default listCollections predicate.
 */
bool acceptAllPred(const BSONObj&) {
    return true;
}

/**
 * Creates a listCollections command obj with an optional filter.
 */
BSONObj createListCollectionsCommandObject(const BSONObj& filter) {
    BSONObjBuilder output;
    output.append("listCollections", 1);
    if (!filter.isEmpty()) {
        output.append("filter", filter);
    }
    return output.obj();
}

}  // namespace

DatabaseCloner::DatabaseCloner(executor::TaskExecutor* executor,
                               ThreadPool* dbWorkThreadPool,
                               const HostAndPort& source,
                               const std::string& dbname,
                               const BSONObj& listCollectionsFilter,
                               const ListCollectionsPredicateFn& listCollectionsPred,
                               StorageInterface* si,
                               const CollectionCallbackFn& collWork,
                               CallbackFn onCompletion)
    : _executor(executor),
      _dbWorkThreadPool(dbWorkThreadPool),
      _source(source),
      _dbname(dbname),
      _listCollectionsFilter(
          listCollectionsFilter.isEmpty()
              ? ListCollectionsFilter::makeTypeCollectionFilter()
              : ListCollectionsFilter::addTypeCollectionFilter(listCollectionsFilter)),
      _listCollectionsPredicate(listCollectionsPred ? listCollectionsPred : acceptAllPred),
      _storageInterface(si),
      _collectionWork(collWork),
      _onCompletion(std::move(onCompletion)),
      _listCollectionsFetcher(_executor,
                              _source,
                              _dbname,
                              createListCollectionsCommandObject(_listCollectionsFilter),
                              [=](const StatusWith<Fetcher::QueryResponse>& result,
                                  Fetcher::NextAction * nextAction,
                                  BSONObjBuilder * getMoreBob) {
                                  _listCollectionsCallback(result, nextAction, getMoreBob);
                              },
                              ReadPreferenceSetting::secondaryPreferredMetadata(),
                              RemoteCommandRequest::kNoTimeout /* find network timeout */,
                              RemoteCommandRequest::kNoTimeout /* getMore network timeout */,
                              RemoteCommandRetryScheduler::makeRetryPolicy(
                                  numInitialSyncListCollectionsAttempts.load(),
                                  executor::RemoteCommandRequest::kNoTimeout,
                                  RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _startCollectionCloner([](CollectionCloner& cloner) { return cloner.startup(); }) {
    // Fetcher throws an exception on null executor.
    invariant(executor);
    uassert(ErrorCodes::BadValue, "db worker thread pool cannot be null", dbWorkThreadPool);
    uassert(ErrorCodes::BadValue, "empty database name", !dbname.empty());
    uassert(ErrorCodes::BadValue, "storage interface cannot be null", si);
    uassert(ErrorCodes::BadValue, "collection callback function cannot be null", collWork);
    uassert(ErrorCodes::BadValue, "callback function cannot be null", _onCompletion);

    _stats.dbname = _dbname;
}

DatabaseCloner::~DatabaseCloner() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

const std::vector<BSONObj>& DatabaseCloner::getCollectionInfos_forTest() const {
    LockGuard lk(_mutex);
    return _collectionInfos;
}

bool DatabaseCloner::isActive() const {
    LockGuard lk(_mutex);
    return _isActive_inlock();
}

bool DatabaseCloner::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

bool DatabaseCloner::_isShuttingDown() const {
    LockGuard lk(_mutex);
    return State::kShuttingDown == _state;
}

Status DatabaseCloner::startup() noexcept {
    UniqueLock lk(_mutex);

    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::InternalError, "database cloner already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress, "database cloner shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress, "database cloner completed");
    }

    MONGO_FAIL_POINT_BLOCK(initialSyncHangBeforeListCollections, customArgs) {
        const auto& data = customArgs.getData();
        const auto databaseElem = data["database"];
        if (!databaseElem || databaseElem.checkAndGetStringData() == _dbname) {
            lk.unlock();
            log() << "initial sync - initialSyncHangBeforeListCollections fail point "
                     "enabled. Blocking until fail point is disabled.";
            while (MONGO_FAIL_POINT(initialSyncHangBeforeListCollections) && !_isShuttingDown()) {
                mongo::sleepsecs(1);
            }
            lk.lock();
        }
    }

    _stats.start = _executor->now();
    LOG(1) << "Scheduling listCollections call for database: " << _dbname;
    Status scheduleResult = _listCollectionsFetcher.schedule();
    if (!scheduleResult.isOK()) {
        error() << "Error scheduling listCollections for database: " << _dbname
                << ", error:" << scheduleResult;
        _state = State::kComplete;
        return scheduleResult;
    }

    return Status::OK();
}

void DatabaseCloner::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    switch (_state) {
        case State::kPreStart:
            // Transition directly from PreStart to Complete if not started yet.
            _state = State::kComplete;
            return;
        case State::kRunning:
            _state = State::kShuttingDown;
            break;
        case State::kShuttingDown:
        case State::kComplete:
            // Nothing to do if we are already in ShuttingDown or Complete state.
            return;
    }

    for (auto&& collectionCloner : _collectionCloners) {
        collectionCloner.shutdown();
    }

    _listCollectionsFetcher.shutdown();
}

DatabaseCloner::Stats DatabaseCloner::getStats() const {
    LockGuard lk(_mutex);
    DatabaseCloner::Stats stats = _stats;
    for (auto&& collectionCloner : _collectionCloners) {
        stats.collectionStats.emplace_back(collectionCloner.getStats());
    }
    return stats;
}

void DatabaseCloner::join() {
    UniqueLock lk(_mutex);
    _condition.wait(lk, [this]() { return !_isActive_inlock(); });
}

void DatabaseCloner::setScheduleDbWorkFn_forTest(const ScheduleDbWorkFn& work) {
    LockGuard lk(_mutex);

    _scheduleDbWorkFn = work;
}

void DatabaseCloner::setStartCollectionClonerFn(
    const StartCollectionClonerFn& startCollectionCloner) {
    _startCollectionCloner = startCollectionCloner;
}

DatabaseCloner::State DatabaseCloner::getState_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _state;
}

void DatabaseCloner::_listCollectionsCallback(const StatusWith<Fetcher::QueryResponse>& result,
                                              Fetcher::NextAction* nextAction,
                                              BSONObjBuilder* getMoreBob) {
    if (!result.isOK()) {
        _finishCallback(result.getStatus().withContext(
            str::stream() << "Error issuing listCollections on db '" << _dbname << "' (host:"
                          << _source.toString()
                          << ")"));
        return;
    }

    auto batchData(result.getValue());
    auto&& documents = batchData.documents;

    UniqueLock lk(_mutex);
    // We may be called with multiple batches leading to a need to grow _collectionInfos.
    _collectionInfos.reserve(_collectionInfos.size() + documents.size());
    std::copy_if(documents.begin(),
                 documents.end(),
                 std::back_inserter(_collectionInfos),
                 _listCollectionsPredicate);
    _stats.collections += _collectionInfos.size();

    // The fetcher will continue to call with kGetMore until an error or the last batch.
    if (*nextAction == Fetcher::NextAction::kGetMore) {
        invariant(getMoreBob);
        getMoreBob->append("getMore", batchData.cursorId);
        getMoreBob->append("collection", batchData.nss.coll());
        return;
    }

    // Nothing to do for an empty database.
    if (_collectionInfos.empty()) {
        _finishCallback_inlock(lk, Status::OK());
        return;
    }

    MONGO_FAIL_POINT_BLOCK(initialSyncHangAfterListCollections, options) {
        const BSONObj& data = options.getData();
        if (data["database"].String() == _dbname) {
            log() << "initial sync - initialSyncHangAfterListCollections fail point "
                     "enabled. Blocking until fail point is disabled.";
            while (MONGO_FAIL_POINT(initialSyncHangAfterListCollections)) {
                mongo::sleepsecs(1);
            }
        }
    }

    _collectionNamespaces.reserve(_collectionInfos.size());
    std::set<std::string> seen;
    for (auto&& info : _collectionInfos) {
        BSONElement nameElement = info.getField(kNameFieldName);
        if (nameElement.eoo()) {
            _finishCallback_inlock(
                lk,
                {ErrorCodes::FailedToParse,
                 str::stream() << "collection info must contain '" << kNameFieldName << "' "
                               << "field : "
                               << info});
            return;
        }
        if (nameElement.type() != mongo::String) {
            _finishCallback_inlock(
                lk,
                {ErrorCodes::TypeMismatch,
                 str::stream() << "'" << kNameFieldName << "' field must be a string: " << info});
            return;
        }
        const std::string collectionName = nameElement.String();
        if (seen.find(collectionName) != seen.end()) {
            _finishCallback_inlock(lk,
                                   {ErrorCodes::Error(51005),
                                    str::stream()
                                        << "collection info contains duplicate collection name "
                                        << "'"
                                        << collectionName
                                        << "': "
                                        << info});
            return;
        }

        BSONElement optionsElement = info.getField(kOptionsFieldName);
        if (optionsElement.eoo()) {
            _finishCallback_inlock(
                lk,
                {ErrorCodes::FailedToParse,
                 str::stream() << "collection info must contain '" << kOptionsFieldName << "' "
                               << "field : "
                               << info});
            return;
        }
        if (!optionsElement.isABSONObj()) {
            _finishCallback_inlock(lk,
                                   Status(ErrorCodes::TypeMismatch,
                                          str::stream() << "'" << kOptionsFieldName
                                                        << "' field must be an object: "
                                                        << info));
            return;
        }
        const BSONObj optionsObj = optionsElement.Obj();
        CollectionOptions options;
        auto statusWithCollectionOptions =
            CollectionOptions::parse(optionsObj, CollectionOptions::parseForStorage);
        if (!statusWithCollectionOptions.isOK()) {
            _finishCallback_inlock(lk, statusWithCollectionOptions.getStatus());
            return;
        }
        options = statusWithCollectionOptions.getValue();

        BSONElement infoElement = info.getField(kInfoFieldName);
        if (infoElement.isABSONObj()) {
            BSONElement uuidElement = infoElement[kUUIDFieldName];
            if (!uuidElement.eoo()) {
                auto res = CollectionUUID::parse(uuidElement);
                if (!res.isOK()) {
                    _finishCallback_inlock(lk, res.getStatus());
                    return;
                }
                options.uuid = res.getValue();
            }
        }
        // TODO(SERVER-27994): Ensure UUID present when FCV >= "3.6".

        seen.insert(collectionName);

        _collectionNamespaces.emplace_back(_dbname, collectionName);
        auto&& nss = *_collectionNamespaces.crbegin();

        try {
            _collectionCloners.emplace_back(
                _executor,
                _dbWorkThreadPool,
                _source,
                nss,
                options,
                [=](const Status& status) { return _collectionClonerCallback(status, nss); },
                _storageInterface,
                collectionClonerBatchSize);
        } catch (const AssertionException& ex) {
            _finishCallback_inlock(lk, ex.toStatus());
            return;
        }
    }

    if (_scheduleDbWorkFn) {
        for (auto&& collectionCloner : _collectionCloners) {
            collectionCloner.setScheduleDbWorkFn_forTest(_scheduleDbWorkFn);
        }
    }

    // Start first collection cloner.
    _currentCollectionClonerIter = _collectionCloners.begin();

    LOG(1) << "    cloning collection " << _currentCollectionClonerIter->getSourceNamespace();

    Status startStatus = _startCollectionCloner(*_currentCollectionClonerIter);
    if (!startStatus.isOK()) {
        LOG(1) << "    failed to start collection cloning on "
               << _currentCollectionClonerIter->getSourceNamespace() << ": " << redact(startStatus);
        _finishCallback_inlock(lk, startStatus);
        return;
    }
}

void DatabaseCloner::_collectionClonerCallback(const Status& status, const NamespaceString& nss) {
    UniqueLock lk(_mutex);
    auto collStatus = Status::OK();

    // Record failure, but do not return just yet, in case we want to do some logging.
    if (!status.isOK()) {
        collStatus = status.withContext(
            str::stream() << "Error cloning collection '" << nss.toString() << "'");
    }

    // Forward collection cloner result to caller.
    lk.unlock();
    _collectionWork(collStatus, nss);
    lk.lock();

    // Failure to clone a collection will stop the database cloner from
    // cloning the rest of the collections in the listCollections result.
    if (!collStatus.isOK()) {
        Status failStatus = {ErrorCodes::InitialSyncFailure, collStatus.toString()};
        _finishCallback_inlock(lk, failStatus);
        return;
    }

    ++_stats.clonedCollections;
    _currentCollectionClonerIter++;

    if (_currentCollectionClonerIter != _collectionCloners.end()) {
        Status startStatus = _startCollectionCloner(*_currentCollectionClonerIter);
        if (!startStatus.isOK()) {
            LOG(1) << "    failed to start collection cloning on "
                   << _currentCollectionClonerIter->getSourceNamespace() << ": "
                   << redact(startStatus);
            _finishCallback_inlock(lk, startStatus);
            return;
        }
        return;
    }

    _finishCallback_inlock(lk, Status::OK());
}

void DatabaseCloner::_finishCallback(const Status& status) {
    _onCompletion(status);
    LockGuard lk(_mutex);
    invariant(_state != State::kComplete);
    _state = State::kComplete;
    _condition.notify_all();
    _stats.end = _executor->now();
    LOG(1) << "    database: " << _dbname << ", stats: " << _stats.toString();
}

void DatabaseCloner::_finishCallback_inlock(UniqueLock& lk, const Status& status) {
    if (lk.owns_lock()) {
        lk.unlock();
    }
    _finishCallback(status);
}

std::string DatabaseCloner::getDBName() const {
    return _dbname;
}

std::string DatabaseCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj DatabaseCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.append("dbname", dbname);
    append(&bob);
    return bob.obj();
}

void DatabaseCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("collections", collections);
    builder->appendNumber("clonedCollections", clonedCollections);
    if (start != Date_t()) {
        builder->appendDate("start", start);
        if (end != Date_t()) {
            builder->appendDate("end", end);
            auto elapsed = end - start;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("elapsedMillis", elapsedMillis);
        }
    }

    for (auto&& collection : collectionStats) {
        BSONObjBuilder collectionBuilder(builder->subobjStart(collection.ns));
        collection.append(&collectionBuilder);
        collectionBuilder.doneFast();
    }
}

std::ostream& operator<<(std::ostream& os, const DatabaseCloner::State& state) {
    switch (state) {
        case DatabaseCloner::State::kPreStart:
            return os << "PreStart";
        case DatabaseCloner::State::kRunning:
            return os << "Running";
        case DatabaseCloner::State::kShuttingDown:
            return os << "ShuttingDown";
        case DatabaseCloner::State::kComplete:
            return os << "Complete";
    }
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
