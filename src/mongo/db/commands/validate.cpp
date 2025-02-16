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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;

MONGO_FAIL_POINT_DEFINE(validateCmdCollectionNotValid);

namespace {

// Protects `_validationQueue`
stdx::mutex _validationMutex;

// Wakes up `_validationQueue`
stdx::condition_variable _validationNotifier;

// Holds the set of full `database.collections` namespace strings in progress.
std::set<std::string> _validationsInProgress;
}  // namespace

class ValidateCmd : public BasicCommand {
public:
    ValidateCmd() : BasicCommand("validate") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Validate contents of a namespace by scanning its data structures for correctness.  "
               "Slow.\n"
               "Add full:true option to do a more thorough check";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool allowsAfterClusterTime(const BSONObj& cmd) const override {
        return false;
    }

    bool canIgnorePrepareConflicts() const override {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::validate);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    //{ validate: "collectionnamewithoutthedbpart" [, full: <bool> } */

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        if (MONGO_FAIL_POINT(validateCmdCollectionNotValid)) {
            result.appendBool("valid", false);
            return true;
        }

        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));

        const bool full = cmdObj["full"].trueValue();

        ValidateCmdLevel level = kValidateNormal;

        if (full) {
            level = kValidateFull;
        }

        if (cmdObj["scandata"]) {
            const char* deprecationWarning =
                "the scandata option is deprecated and will be removed in a future release";
            result.append("note", deprecationWarning);
        }

        if (!serverGlobalParams.quiet.load()) {
            LOG(0) << "CMD: validate " << nss.ns();
        }

        AutoGetDb ctx(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLk(opCtx, nss, MODE_X);
        Collection* collection = ctx.getDb() ? ctx.getDb()->getCollection(opCtx, nss) : nullptr;
        if (!collection) {
            if (ctx.getDb() && ViewCatalog::get(ctx.getDb())->lookup(opCtx, nss.ns())) {
                uasserted(ErrorCodes::CommandNotSupportedOnView, "Cannot validate a view");
            }

            uasserted(ErrorCodes::NamespaceNotFound, "ns not found");
        }

        result.append("ns", nss.ns());

        // Only one validation per collection can be in progress, the rest wait in order.
        {
            stdx::unique_lock<stdx::mutex> lock(_validationMutex);
            try {
                while (_validationsInProgress.find(nss.ns()) != _validationsInProgress.end()) {
                    opCtx->waitForConditionOrInterrupt(_validationNotifier, lock);
                }
            } catch (AssertionException& e) {
                CommandHelpers::appendCommandStatusNoThrow(
                    result,
                    {ErrorCodes::CommandFailed,
                     str::stream() << "Exception during validation: " << e.toString()});
                return false;
            }

            _validationsInProgress.insert(nss.ns());
        }

        ON_BLOCK_EXIT([&] {
            stdx::lock_guard<stdx::mutex> lock(_validationMutex);
            _validationsInProgress.erase(nss.ns());
            _validationNotifier.notify_all();
        });

        // TODO SERVER-30357: Add support for background validation.
        const bool background = false;

        ValidateResults results;
        Status status = collection->validate(opCtx, level, background, &results, &result);
        if (!status.isOK()) {
            return CommandHelpers::appendCommandStatusNoThrow(result, status);
        }

        CollectionOptions opts =
            DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, collection->ns());

        // All collections must have a UUID.
        if (!opts.uuid) {
            results.errors.push_back(str::stream() << "UUID missing on collection " << nss.ns()
                                                   << " but SchemaVersion=3.6");
            results.valid = false;
        }

        if (!full) {
            results.warnings.push_back(
                "Some checks omitted for speed. use {full:true} option to do more thorough scan.");
        }

        result.appendBool("valid", results.valid);
        result.append("warnings", results.warnings);
        result.append("errors", results.errors);
        result.append("extraIndexEntries", results.extraIndexEntries);
        result.append("missingIndexEntries", results.missingIndexEntries);

        if (!results.valid) {
            result.append("advice",
                          "A corrupt namespace has been detected. See "
                          "http://dochub.mongodb.org/core/data-recovery for recovery steps.");
        }

        return true;
    }

} validateCmd;
}
