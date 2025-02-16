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

#include "mongo/db/commands/mr_common.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

namespace mr {

namespace {
Rarely nonAtomicDeprecationSampler;  // Used to occasionally log deprecation messages.
}  // namespace

OutputOptions parseOutputOptions(const std::string& dbname, const BSONObj& cmdObj) {
    OutputOptions outputOptions;

    outputOptions.outNonAtomic = false;
    if (cmdObj["out"].type() == String) {
        outputOptions.collectionName = cmdObj["out"].String();
        outputOptions.outType = OutputType::kReplace;
    } else if (cmdObj["out"].type() == Object) {
        BSONObj o = cmdObj["out"].embeddedObject();

        if (o.hasElement("normal")) {
            outputOptions.outType = OutputType::kReplace;
            outputOptions.collectionName = o["normal"].String();
        } else if (o.hasElement("replace")) {
            outputOptions.outType = OutputType::kReplace;
            outputOptions.collectionName = o["replace"].String();
        } else if (o.hasElement("merge")) {
            outputOptions.outType = OutputType::kMerge;
            outputOptions.collectionName = o["merge"].String();
        } else if (o.hasElement("reduce")) {
            outputOptions.outType = OutputType::kReduce;
            outputOptions.collectionName = o["reduce"].String();
        } else if (o.hasElement("inline")) {
            outputOptions.outType = OutputType::kInMemory;
        } else {
            uasserted(13522,
                      str::stream() << "please specify one of "
                                    << "[replace|merge|reduce|inline] in 'out' object");
        }

        if (o.hasElement("db")) {
            outputOptions.outDB = o["db"].String();
        }
        if (o.hasElement("nonAtomic")) {
            outputOptions.outNonAtomic = o["nonAtomic"].Bool();
            if (outputOptions.outNonAtomic) {
                uassert(15895,
                        "nonAtomic option cannot be used with this output type",
                        (outputOptions.outType == OutputType::kReduce ||
                         outputOptions.outType == OutputType::kMerge));
            } else if (nonAtomicDeprecationSampler.tick()) {
                warning() << "Setting out.nonAtomic to false in MapReduce is deprecated.";
            }
        }
    } else {
        uasserted(13606, "'out' has to be a string or an object");
    }

    if (outputOptions.outType != OutputType::kInMemory) {
        const StringData outDb(outputOptions.outDB.empty() ? dbname : outputOptions.outDB);
        const NamespaceString nss(outDb, outputOptions.collectionName);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid 'out' namespace: " << nss.ns(),
                nss.isValid());
        outputOptions.finalNamespace = std::move(nss);
    }

    return outputOptions;
}

void addPrivilegesRequiredForMapReduce(const BasicCommand* commandTemplate,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
    OutputOptions outputOptions = parseOutputOptions(dbname, cmdObj);

    ResourcePattern inputResource(commandTemplate->parseResourcePattern(dbname, cmdObj));
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid input resource " << inputResource.toString(),
            inputResource.isExactNamespacePattern());
    out->push_back(Privilege(inputResource, ActionType::find));

    if (outputOptions.outType != OutputType::kInMemory) {
        ActionSet outputActions;
        outputActions.addAction(ActionType::insert);
        if (outputOptions.outType == OutputType::kReplace) {
            outputActions.addAction(ActionType::remove);
        } else {
            outputActions.addAction(ActionType::update);
        }

        if (shouldBypassDocumentValidationForCommand(cmdObj)) {
            outputActions.addAction(ActionType::bypassDocumentValidation);
        }

        ResourcePattern outputResource(
            ResourcePattern::forExactNamespace(NamespaceString(outputOptions.finalNamespace)));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target namespace " << outputResource.ns().ns(),
                outputResource.ns().isValid());

        // TODO: check if outputNs exists and add createCollection privilege if not
        out->push_back(Privilege(outputResource, outputActions));
    }
}

bool mrSupportsWriteConcern(const BSONObj& cmd) {
    if (!cmd.hasField("out")) {
        return false;
    } else if (cmd["out"].type() == Object && cmd["out"].Obj().hasField("inline")) {
        return false;
    } else {
        return true;
    }
}
}
}
