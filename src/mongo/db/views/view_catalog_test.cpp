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

#include <functional>
#include <memory>
#include <set>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

constexpr auto kLargeString =
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000";
const auto kOneKiBMatchStage = BSON("$match" << BSON("data" << kLargeString));
const auto kTinyMatchStage = BSON("$match" << BSONObj());

class DurableViewCatalogDummy final : public DurableViewCatalog {
public:
    explicit DurableViewCatalogDummy() : _upsertCount(0), _iterateCount(0) {}
    static const std::string name;

    using Callback = std::function<Status(const BSONObj& view)>;
    virtual void iterate(OperationContext* opCtx, Callback callback) {
        ++_iterateCount;
    }
    virtual void iterateIgnoreInvalidEntries(OperationContext* opCtx, Callback callback) {
        ++_iterateCount;
    }
    virtual void upsert(OperationContext* opCtx, const NamespaceString& name, const BSONObj& view) {
        ++_upsertCount;
    }
    virtual void remove(OperationContext* opCtx, const NamespaceString& name) {}
    virtual const std::string& getName() const {
        return name;
    };

    int getUpsertCount() {
        return _upsertCount;
    }

    int getIterateCount() {
        return _iterateCount;
    }

private:
    int _upsertCount;
    int _iterateCount;
};

const std::string DurableViewCatalogDummy::name = "dummy";

class ViewCatalogFixture : public unittest::Test {
public:
    ViewCatalogFixture()
        : _queryServiceContext(std::make_unique<QueryTestServiceContext>()),
          opCtx(_queryServiceContext->makeOperationContext()),
          viewCatalog(std::move(durableViewCatalogUnique)) {}

private:
    std::unique_ptr<QueryTestServiceContext> _queryServiceContext;

protected:
    ServiceContext* getServiceContext() const {
        return _queryServiceContext->getServiceContext();
    }

    std::unique_ptr<DurableViewCatalogDummy> durableViewCatalogUnique =
        std::make_unique<DurableViewCatalogDummy>();
    DurableViewCatalogDummy* durableViewCatalog = durableViewCatalogUnique.get();
    ServiceContext::UniqueOperationContext opCtx;
    ViewCatalog viewCatalog;
    const BSONArray emptyPipeline;
    const BSONObj emptyCollation;
};

// For tests which need to run in a replica set context.
class ReplViewCatalogFixture : public ViewCatalogFixture {
public:
    void setUp() override {
        Test::setUp();
        auto service = getServiceContext();
        repl::ReplSettings settings;

        settings.setReplSetString("viewCatalogTestSet/node1:12345");

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service, settings);

        // Ensure that we are primary.
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
    }
};

TEST_F(ViewCatalogFixture, CreateExistingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewOnDifferentDatabase) {
    const NamespaceString viewName("db1.view");
    const NamespaceString viewOn("db2.coll");

    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CanCreateViewWithExprPredicate) {
    const NamespaceString viewOn("db.coll");
    ASSERT_OK(viewCatalog.createView(opCtx.get(),
                                     NamespaceString("db.view1"),
                                     viewOn,
                                     BSON_ARRAY(BSON("$match" << BSON("$expr" << 1))),
                                     emptyCollation));

    ASSERT_OK(viewCatalog.createView(
        opCtx.get(),
        NamespaceString("db.view2"),
        viewOn,
        BSON_ARRAY(
            BSON("$facet" << BSON("output" << BSON_ARRAY(BSON("$match" << BSON("$expr" << 1)))))),
        emptyCollation));
}

TEST_F(ViewCatalogFixture, CanCreateViewWithJSONSchemaPredicate) {
    const NamespaceString viewOn("db.coll");
    ASSERT_OK(viewCatalog.createView(
        opCtx.get(),
        NamespaceString("db.view1"),
        viewOn,
        BSON_ARRAY(BSON("$match" << BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("x"))))),
        emptyCollation));

    ASSERT_OK(viewCatalog.createView(
        opCtx.get(),
        NamespaceString("db.view2"),
        viewOn,
        BSON_ARRAY(BSON(
            "$facet" << BSON(
                "output" << BSON_ARRAY(BSON(
                    "$match" << BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("x")))))))),
        emptyCollation));
}

TEST_F(ViewCatalogFixture, CanCreateViewWithLookupUsingPipelineSyntax) {
    const NamespaceString viewOn("db.coll");
    ASSERT_OK(viewCatalog.createView(opCtx.get(),
                                     NamespaceString("db.view"),
                                     viewOn,
                                     BSON_ARRAY(BSON("$lookup" << BSON("from"
                                                                       << "fcoll"
                                                                       << "as"
                                                                       << "as"
                                                                       << "pipeline"
                                                                       << BSONArray()))),
                                     emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewWithPipelineFailsOnInvalidStageName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto invalidPipeline = BSON_ARRAY(BSON("INVALID_STAGE_NAME" << 1));
    ASSERT_THROWS(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, invalidPipeline, emptyCollation),
        AssertionException);
}

TEST_F(ReplViewCatalogFixture, CreateViewWithPipelineFailsOnIneligibleStage) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    // $changeStream cannot be used in a view definition pipeline.
    auto invalidPipeline = BSON_ARRAY(BSON("$changeStream" << BSONObj()));

    ASSERT_THROWS_CODE(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, invalidPipeline, emptyCollation),
        AssertionException,
        ErrorCodes::OptionNotSupportedOnView);
}

TEST_F(ReplViewCatalogFixture, CreateViewWithPipelineFailsOnIneligibleStagePersistentWrite) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    // $out cannot be used in a view definition pipeline.
    auto invalidPipeline = BSON_ARRAY(BSON("$out"
                                           << "someOtherCollection"));

    ASSERT_THROWS_CODE(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, invalidPipeline, emptyCollation),
        AssertionException,
        ErrorCodes::OptionNotSupportedOnView);

    invalidPipeline = BSON_ARRAY(BSON("$merge"
                                      << "someOtherCollection"));

    ASSERT_THROWS_CODE(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, invalidPipeline, emptyCollation),
        AssertionException,
        ErrorCodes::OptionNotSupportedOnView);
}

TEST_F(ViewCatalogFixture, CreateViewOnInvalidCollectionName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.$coll");

    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, ExceedMaxViewDepthInOrder) {
    const char* ns = "db.view";
    int i = 0;

    for (; i < ViewGraph::kMaxViewDepth; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    const NamespaceString viewName(str::stream() << ns << i);
    const NamespaceString viewOn(str::stream() << ns << (i + 1));

    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, ExceedMaxViewDepthByJoining) {
    const char* ns = "db.view";
    int i = 0;
    int size = ViewGraph::kMaxViewDepth * 2 / 3;

    for (; i < size; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    for (i = 1; i < size + 1; i++) {
        const NamespaceString viewName(str::stream() << ns << (size + i));
        const NamespaceString viewOn(str::stream() << ns << (size + i + 1));

        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    const NamespaceString viewName(str::stream() << ns << size);
    const NamespaceString viewOn(str::stream() << ns << (size + 1));

    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewCycles) {
    {
        const NamespaceString viewName("db.view1");
        const NamespaceString viewOn("db.view1");

        ASSERT_NOT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    {
        const NamespaceString view1("db.view1");
        const NamespaceString view2("db.view2");
        const NamespaceString view3("db.view3");

        ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, view2, emptyPipeline, emptyCollation));
        ASSERT_OK(viewCatalog.createView(opCtx.get(), view2, view3, emptyPipeline, emptyCollation));
        ASSERT_NOT_OK(
            viewCatalog.createView(opCtx.get(), view3, view1, emptyPipeline, emptyCollation));
    }
}

TEST_F(ViewCatalogFixture, CanSuccessfullyCreateViewWhosePipelineIsExactlyAtMaxSizeInBytes) {
    ASSERT_EQ(ViewGraph::kMaxViewPipelineSizeBytes % kOneKiBMatchStage.objsize(), 0);

    BSONArrayBuilder builder(ViewGraph::kMaxViewPipelineSizeBytes);
    int pipelineSize = 0;
    for (; pipelineSize < ViewGraph::kMaxViewPipelineSizeBytes;
         pipelineSize += kOneKiBMatchStage.objsize()) {
        builder << kOneKiBMatchStage;
    }

    ASSERT_EQ(pipelineSize, ViewGraph::kMaxViewPipelineSizeBytes);

    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");
    const BSONObj collation;

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, builder.arr(), collation));
}

TEST_F(ViewCatalogFixture, CannotCreateViewWhosePipelineExceedsMaxSizeInBytes) {
    // Fill the builder to exactly the maximum size, then push it just over the limit by adding an
    // additional tiny match stage.
    BSONArrayBuilder builder(ViewGraph::kMaxViewPipelineSizeBytes);
    for (int pipelineSize = 0; pipelineSize < ViewGraph::kMaxViewPipelineSizeBytes;
         pipelineSize += kOneKiBMatchStage.objsize()) {
        builder << kOneKiBMatchStage;
    }
    builder << kTinyMatchStage;

    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");
    const BSONObj collation;

    ASSERT_NOT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, builder.arr(), collation));
}

TEST_F(ViewCatalogFixture, CannotCreateViewIfItsFullyResolvedPipelineWouldExceedMaxSizeInBytes) {
    BSONArrayBuilder builder1;
    BSONArrayBuilder builder2;

    for (int pipelineSize = 0; pipelineSize < ViewGraph::kMaxViewPipelineSizeBytes;
         pipelineSize += (kOneKiBMatchStage.objsize() * 2)) {
        builder1 << kOneKiBMatchStage;
        builder2 << kOneKiBMatchStage;
    }
    builder2 << kTinyMatchStage;

    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString viewOn("db.coll");
    const BSONObj collation1;
    const BSONObj collation2;

    ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, viewOn, builder1.arr(), collation1));
    ASSERT_NOT_OK(viewCatalog.createView(opCtx.get(), view2, view1, builder2.arr(), collation2));
}

TEST_F(ViewCatalogFixture, DropMissingView) {
    NamespaceString viewName("db.view");
    ASSERT_NOT_OK(viewCatalog.dropView(opCtx.get(), viewName));
}

TEST_F(ViewCatalogFixture, ModifyMissingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_NOT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
}

TEST_F(ViewCatalogFixture, ModifyViewOnDifferentDatabase) {
    const NamespaceString viewName("db1.view");
    const NamespaceString viewOn("db2.coll");

    ASSERT_NOT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
}

TEST_F(ViewCatalogFixture, ModifyViewOnInvalidCollectionName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.$coll");

    ASSERT_NOT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
}

TEST_F(ReplViewCatalogFixture, ModifyViewWithPipelineFailsOnIneligibleStage) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto validPipeline = BSON_ARRAY(BSON("$match" << BSON("_id" << 1)));
    auto invalidPipeline = BSON_ARRAY(BSON("$changeStream" << BSONObj()));

    // Create the initial, valid view.
    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, validPipeline, emptyCollation));

    // Now attempt to replace it with a pipeline containing $changeStream.
    ASSERT_THROWS_CODE(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, invalidPipeline),
                       AssertionException,
                       ErrorCodes::OptionNotSupportedOnView);
}

TEST_F(ViewCatalogFixture, LookupMissingView) {
    ASSERT(!viewCatalog.lookup(opCtx.get(), "db.view"_sd));
}

TEST_F(ViewCatalogFixture, LookupExistingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));

    ASSERT(viewCatalog.lookup(opCtx.get(), "db.view"_sd));
}

TEST_F(ViewCatalogFixture, LookupRIDExistingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    auto& collectionCatalog = CollectionCatalog::get(opCtx.get());
    ASSERT(collectionCatalog.lookupResourceName(resourceID).get() == "db.view");
}

TEST_F(ViewCatalogFixture, LookupRIDExistingViewRollback) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");
    {
        WriteUnitOfWork wunit(opCtx.get());
        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }
    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    auto& collectionCatalog = CollectionCatalog::get(opCtx.get());
    ASSERT(!collectionCatalog.lookupResourceName(resourceID));
}

TEST_F(ViewCatalogFixture, LookupRIDAfterDrop) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(viewCatalog.dropView(opCtx.get(), viewName));

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    auto& collectionCatalog = CollectionCatalog::get(opCtx.get());
    ASSERT(!collectionCatalog.lookupResourceName(resourceID));
}

TEST_F(ViewCatalogFixture, LookupRIDAfterDropRollback) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    auto& collectionCatalog = CollectionCatalog::get(opCtx.get());
    {
        WriteUnitOfWork wunit(opCtx.get());
        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
        ASSERT(collectionCatalog.lookupResourceName(resourceID).get() == viewName.ns());
        wunit.commit();
    }

    {
        WriteUnitOfWork wunit(opCtx.get());
        ASSERT_OK(viewCatalog.dropView(opCtx.get(), viewName));
    }

    ASSERT(collectionCatalog.lookupResourceName(resourceID).get() == viewName.ns());
}

TEST_F(ViewCatalogFixture, LookupRIDAfterModify) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    auto& collectionCatalog = CollectionCatalog::get(opCtx.get());
    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
    ASSERT(collectionCatalog.lookupResourceName(resourceID).get() == viewName.ns());
}

TEST_F(ViewCatalogFixture, LookupRIDAfterModifyRollback) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    auto& collectionCatalog = CollectionCatalog::get(opCtx.get());
    {
        WriteUnitOfWork wunit(opCtx.get());
        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
        ASSERT(collectionCatalog.lookupResourceName(resourceID).get() == viewName.ns());
        wunit.commit();
    }
    {
        WriteUnitOfWork wunit(opCtx.get());
        ASSERT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
        ASSERT(collectionCatalog.lookupResourceName(resourceID).get() == viewName.ns());
    }
    ASSERT(collectionCatalog.lookupResourceName(resourceID).get() == viewName.ns());
}

TEST_F(ViewCatalogFixture, CreateViewThenDropAndLookup) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(viewCatalog.dropView(opCtx.get(), viewName));

    ASSERT(!viewCatalog.lookup(opCtx.get(), "db.view"_sd));
}

TEST_F(ViewCatalogFixture, ModifyTenTimes) {
    const char* ns = "db.view";
    int i;

    for (i = 0; i < 5; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    for (i = 0; i < 5; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
    }

    ASSERT_EQ(10, durableViewCatalog->getUpsertCount());
}

TEST_F(ViewCatalogFixture, Iterate) {
    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString view3("db.view3");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view2, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view3, viewOn, emptyPipeline, emptyCollation));

    std::set<std::string> viewNames = {"db.view1", "db.view2", "db.view3"};

    viewCatalog.iterate(opCtx.get(), [&viewNames](const ViewDefinition& view) {
        std::string name = view.name().toString();
        ASSERT(viewNames.end() != viewNames.find(name));
        viewNames.erase(name);
    });

    ASSERT(viewNames.empty());
}

TEST_F(ViewCatalogFixture, ResolveViewCorrectPipeline) {
    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString view3("db.view3");
    const NamespaceString viewOn("db.coll");
    BSONArrayBuilder pipeline1;
    BSONArrayBuilder pipeline3;
    BSONArrayBuilder pipeline2;

    pipeline1 << BSON("$match" << BSON("foo" << 1));
    pipeline2 << BSON("$match" << BSON("foo" << 2));
    pipeline3 << BSON("$match" << BSON("foo" << 3));

    ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, viewOn, pipeline1.arr(), emptyCollation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view2, view1, pipeline2.arr(), emptyCollation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view3, view2, pipeline3.arr(), emptyCollation));

    auto resolvedView = viewCatalog.resolveView(opCtx.get(), view3);
    ASSERT(resolvedView.isOK());

    std::vector<BSONObj> expected = {BSON("$match" << BSON("foo" << 1)),
                                     BSON("$match" << BSON("foo" << 2)),
                                     BSON("$match" << BSON("foo" << 3))};

    std::vector<BSONObj> result = resolvedView.getValue().getPipeline();

    ASSERT_EQ(expected.size(), result.size());

    for (uint32_t i = 0; i < expected.size(); i++) {
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(expected[i] == result[i]));
    }
}

TEST_F(ViewCatalogFixture, ResolveViewOnCollectionNamespace) {
    const NamespaceString collectionNamespace("db.coll");

    auto resolvedView = uassertStatusOK(viewCatalog.resolveView(opCtx.get(), collectionNamespace));

    ASSERT_EQ(resolvedView.getNamespace(), collectionNamespace);
    ASSERT_EQ(resolvedView.getPipeline().size(), 0U);
}

TEST_F(ViewCatalogFixture, ResolveViewCorrectlyExtractsDefaultCollation) {
    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString viewOn("db.coll");
    BSONArrayBuilder pipeline1;
    BSONArrayBuilder pipeline2;

    pipeline1 << BSON("$match" << BSON("foo" << 1));
    pipeline2 << BSON("$match" << BSON("foo" << 2));

    BSONObj collation = BSON("locale"
                             << "mock_reverse_string");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, viewOn, pipeline1.arr(), collation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view2, view1, pipeline2.arr(), collation));

    auto resolvedView = viewCatalog.resolveView(opCtx.get(), view2);
    ASSERT(resolvedView.isOK());

    ASSERT_EQ(resolvedView.getValue().getNamespace(), viewOn);

    std::vector<BSONObj> expected = {BSON("$match" << BSON("foo" << 1)),
                                     BSON("$match" << BSON("foo" << 2))};
    std::vector<BSONObj> result = resolvedView.getValue().getPipeline();
    ASSERT_EQ(expected.size(), result.size());
    for (uint32_t i = 0; i < expected.size(); i++) {
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(expected[i] == result[i]));
    }

    auto expectedCollation =
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation);
    ASSERT_OK(expectedCollation.getStatus());
    ASSERT_BSONOBJ_EQ(resolvedView.getValue().getDefaultCollation(),
                      expectedCollation.getValue()->getSpec().toBSON());
}

TEST_F(ViewCatalogFixture, InvalidateThenReload) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_EQ(1, durableViewCatalog->getIterateCount());

    ASSERT(viewCatalog.lookup(opCtx.get(), "db.view"_sd));
    ASSERT_EQ(1, durableViewCatalog->getIterateCount());

    viewCatalog.invalidate();
    ASSERT_OK(viewCatalog.reloadIfNeeded(opCtx.get()));
    ASSERT_EQ(2, durableViewCatalog->getIterateCount());
}
}  // namespace
}  // namespace mongo
