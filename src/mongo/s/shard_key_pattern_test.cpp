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

#include "mongo/s/shard_key_pattern.h"

#include "mongo/db/hasher.h"
#include "mongo/db/json.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::string;

TEST(ShardKeyPattern, SingleFieldShardKeyPatternsValidityCheck) {
    ShardKeyPattern(BSON("a" << 1));
    ShardKeyPattern(BSON("a" << 1.0f));
    ShardKeyPattern(BSON("a" << (long long)1L));
    ShardKeyPattern(BSON("a"
                         << "hashed"));

    ASSERT_THROWS(ShardKeyPattern(BSONObj()), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << -1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << -1.0)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a"
                                       << "1")),
                  DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a"
                                       << "hash")),
                  DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("" << 1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("." << 1)), DBException);
}

TEST(ShardKeyPattern, CompositeShardKeyPatternsValidityCheck) {
    ShardKeyPattern(BSON("a" << 1 << "b" << 1));
    ShardKeyPattern(BSON("a" << 1.0f << "b" << 1.0));
    ShardKeyPattern(BSON("a" << 1 << "b" << 1.0 << "c" << 1.0f));

    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "b" << -1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "b"
                                           << "1")),
                  DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "b." << 1.0)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "" << 1.0)), DBException);
}

TEST(ShardKeyPattern, NestedShardKeyPatternsValidtyCheck) {
    ShardKeyPattern(BSON("a.b" << 1));
    ShardKeyPattern(BSON("a.b.c.d" << 1.0));
    ShardKeyPattern(BSON("a" << 1 << "c.d" << 1.0 << "e.f.g" << 1.0f));
    ShardKeyPattern(BSON("a" << 1 << "a.b" << 1.0 << "a.b.c" << 1.0f));

    ASSERT_THROWS(ShardKeyPattern(BSON("a.b" << -1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << BSON("b" << 1))), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a.b." << 1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a.b.." << 1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a..b" << 1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "a.b." << 1.0)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << BSON("b" << 1) << "c.d" << 1.0)), DBException);
}

TEST(ShardKeyPattern, IsShardKey) {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0f));

    ASSERT(pattern.isShardKey(BSON("a.b" << 10 << "c" << 30)));
    ASSERT(pattern.isShardKey(BSON("c" << 30 << "a.b" << 10)));

    ASSERT(!pattern.isShardKey(BSON("b" << 10)));
    ASSERT(!pattern.isShardKey(BSON("a" << 10 << "c" << 30)));
    ASSERT(!pattern.isShardKey(BSON("a" << BSON("b" << 10) << "c" << 30)));
}

static BSONObj normKey(const ShardKeyPattern& pattern, const BSONObj& doc) {
    return pattern.normalizeShardKey(doc);
}

TEST(ShardKeyPattern, NormalizeShardKey) {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0f));

    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("a.b" << 10 << "c" << 30)),
                      BSON("a.b" << 10 << "c" << 30));
    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("c" << 30 << "a.b" << 10)),
                      BSON("a.b" << 10 << "c" << 30));
    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("a.b" << BSON("$notAndOperator" << 10) << "c" << 30)),
                      BSON("a.b" << BSON("$notAndOperator" << 10) << "c" << 30));
    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("a.b" << BSON("$gt" << 10) << "c" << 30)),
                      BSON("a.b" << BSON("$gt" << 10) << "c" << 30));

    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("b" << 10)), BSONObj());
    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("a" << 10 << "c" << 30)), BSONObj());
}

static BSONObj docKey(const ShardKeyPattern& pattern, const BSONObj& doc) {
    return pattern.extractShardKeyFromDoc(doc);
}

TEST(ShardKeyPattern, ExtractDocShardKeySingle) {
    //
    // Single field ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:'20'}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:10}, c:30}")), fromjson("{a:{b:10}}"));
    const BSONRegEx regex("abc");
    ASSERT_BSONOBJ_EQ(docKey(pattern,
                             BSON("a" << regex << "b"
                                      << "20")),
                      BSON("a" << regex));
    const BSONObj ref = BSON("$ref"
                             << "coll"
                             << "$id"
                             << 1);
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << ref)), BSON("a" << ref));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{$dollarPrefixKey:true}}")),
                      fromjson("{a:{$dollarPrefixKey:true}}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{$gt:10}}")), fromjson("{a:{$gt:10}}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{$gt:{$dollarPrefixKey:10}}}}")),
                      fromjson("{a:{$gt:{$dollarPrefixKey:10}}}}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, BSONObj()), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{b:10}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("" << 10)), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[1,2]}")), BSONObj());
    // BSONObjIterator breaks this for now
    // ASSERT_EQUALS(docKey(pattern, BSON("a" << 10 << "a" << 20)), BSONObj());
}

TEST(ShardKeyPattern, ExtractDocShardKeyCompound) {
    //
    // Compound ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1 << "b" << 1.0));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:'20'}")), fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:'20', c:30}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern,
                             BSON("c" << 30 << "b"
                                      << "20"
                                      << "a"
                                      << 10)),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:{$dollarPrefixKey:true}}")),
                      fromjson("{a:10, b:{$dollarPrefixKey:true}}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:{$gt:20}}")),
                      fromjson("{a:10, b:{$gt:20}}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:[1, 2]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{b:20}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern,
                             BSON("" << 10 << "b"
                                     << "20")),
                      BSONObj());

    // Ordering
    ASSERT_EQUALS(docKey(pattern, BSON("b" << 20 << "a" << 10)).firstElement().numberInt(), 10);
}

TEST(ShardKeyPattern, ExtractDocShardKeyNested) {
    //
    // Nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0f));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:10}, c:30}")), fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{d:[1,2],b:10},c:30,d:40}")),
                      fromjson("{'a.b':10, c:30}"));
    const BSONObj ref = BSON("$ref"
                             << "coll"
                             << "$id"
                             << 1);
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("b" << ref) << "c" << 30)),
                      BSON("a.b" << ref << "c" << 30));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{d:40}, c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{b:10}, {b:20}], c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:[10, 20]}, c:30}")), BSONObj());
}

TEST(ShardKeyPattern, ExtractDocShardKeyDeepNested) {
    //
    // Deeply nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b.c" << 1));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:{c:10}}}")), fromjson("{'a.b.c':10}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{b:{c:10}}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:[{c:10}]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:{c:[10, 20]}}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:[{c:10}, {c:20}]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{b:{c:10}},{b:{c:20}}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{b:[{c:10},{c:20}]},{b:[{c:30},{c:40}]}]}}")),
                      BSONObj());
}

TEST(ShardKeyPattern, ExtractDocShardKeyHashed) {
    //
    // Hashed ShardKeyPattern
    //

    const string value = "12345";
    const BSONObj bsonValue = BSON("" << value);
    const long long hashValue =
        BSONElementHasher::hash64(bsonValue.firstElement(), BSONElementHasher::DEFAULT_HASH_SEED);

    ShardKeyPattern pattern(BSON("a.b"
                                 << "hashed"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("b" << value))), BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("b" << value) << "c" << 30)),
                      BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("c" << 30 << "b" << value))),
                      BSON("a.b" << hashValue));

    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("c" << value))), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("b" << BSON_ARRAY(value)))), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON_ARRAY(BSON("b" << value)))), BSONObj());
}

static BSONObj queryKey(const ShardKeyPattern& pattern, const BSONObj& query) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    StatusWith<BSONObj> status = pattern.extractShardKeyFromQuery(opCtx.get(), query);
    if (!status.isOK())
        return BSONObj();
    return status.getValue();
}

TEST(ShardKeyPattern, ExtractQueryShardKeySingle) {
    //
    // Single field ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:'20'}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:10}, c:30}")), fromjson("{a:{b:10}}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:{$gt:20}}")), fromjson("{a:10}"));

    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$gt:10}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10,b:{$invalid:'20'}}")), BSONObj());

    // Doc key extraction shouldn't work with query
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$eq:[10, 20]}, c:30}")), BSONObj());

    // $eq/$or/$and/$all
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$eq:10}}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{$eq:10}}]}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:{$eq:10}},{b:'20'}]}")),
                      fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$all:[10]}}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{$eq:10}},{a:10}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:10},{a:10}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$all:[10,10]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{$eq:10}},{b:'20'}]}")), BSONObj());

    // Regex can't be extracted from query
    const BSONRegEx regex("abc");
    ASSERT_BSONOBJ_EQ(queryKey(pattern,
                               BSON("a" << regex << "b"
                                        << "20")),
                      BSONObj());
}

TEST(ShardKeyPattern, ExtractQueryShardKeyCompound) {
    //
    // Compound ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1 << "b" << 1.0));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:'20'}")), fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:'20', c:30}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern,
                               BSON("c" << 30 << "b"
                                        << "20"
                                        << "a"
                                        << 10)),
                      fromjson("{a:10, b:'20'}"));

    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:[1, 2]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10,b:{$invalid:true}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{b:20}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern,
                               BSON("" << 10 << "b"
                                       << "20")),
                      BSONObj());

    // $eq/$or/$and/$all
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$eq:10}, b:{$all:['20']}}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:{$eq:10},b:{$eq:'20'}}]}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:{$eq:10}},{b:{$eq:'20'}}]}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:{$gt:20}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{$eq:10}},{b:'20'}]}")), BSONObj());

    // Ordering
    ASSERT_EQUALS(queryKey(pattern, BSON("b" << 20 << "a" << 10)).firstElement().numberInt(), 10);
}

TEST(ShardKeyPattern, ExtractQueryShardKeyNested) {
    //
    // Nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0f));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:10}, c:30}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b':{$eq:10}, c:30, d:40}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{'a.b':10, c:30, d:40}]}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b':{$all:[10]}, c:30, d:40}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:10,d:40}, c:30}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{'a.b':{$eq:10}}, {c:30}]}")),
                      fromjson("{'a.b':10, c:30}"));

    // Nested $eq is actually a document element
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:{$eq:10}}, c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:{b:{$eq:10}}},{c:30}]}")), BSONObj());

    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{b:{$eq:10}}},{c:30}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:10}, c:{$gt:30}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{d:40}, c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{b:10}, {b:20}],c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:{$eq:[10, 20]}},c:30}")), BSONObj());
}

TEST(ShardKeyPattern, ExtractQueryShardKeyDeepNested) {
    //
    // Deeply nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b.c" << 1));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:{c:10}}}")), fromjson("{'a.b.c':10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b.c':10}")), fromjson("{'a.b.c':10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b.c':{$eq:10}}")), fromjson("{'a.b.c':10}"));

    // Arrays at any nesting level means we can't extract a shard key
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b.c':[10]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b':[{c:10}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{b:{c:10}}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:[{c:10}]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:{c:[10, 20]}}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:[{c:10}, {c:20}]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{b:{c:10}},{b:{c:20}}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{b:[{c:10},{c:20}]},{b:[{c:30},{c:40}]}]}}")),
                      BSONObj());
}

TEST(ShardKeyPattern, ExtractQueryShardKeyHashed) {
    //
    // Hashed ShardKeyPattern
    //

    const string value = "12345";
    const BSONObj bsonValue = BSON("" << value);
    const long long hashValue =
        BSONElementHasher::hash64(bsonValue.firstElement(), BSONElementHasher::DEFAULT_HASH_SEED);

    // Hashed works basically the same as non-hashed, but applies the hash function at the end
    ShardKeyPattern pattern(BSON("a.b"
                                 << "hashed"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a.b" << value)), BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("b" << value))), BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a.b" << BSON("$eq" << value))),
                      BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("b" << value) << "c" << 30)),
                      BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("c" << 30 << "b" << value))),
                      BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern,  //
                               BSON("$and" << BSON_ARRAY(BSON("a.b" << BSON("$eq" << value))))),
                      BSON("a.b" << hashValue));

    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("b" << BSON("$eq" << value)))), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a.b" << BSON("$gt" << value))), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("c" << value))), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("b" << BSON_ARRAY(value)))), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON_ARRAY(BSON("b" << value)))), BSONObj());
}

static bool indexComp(const ShardKeyPattern& pattern, const BSONObj& indexPattern) {
    return pattern.isUniqueIndexCompatible(indexPattern);
}

TEST(ShardKeyPattern, UniqueIndexCompatibleSingle) {
    //
    // Single field ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1));
    ASSERT(indexComp(pattern, BSON("a" << 1)));
    ASSERT(indexComp(pattern, BSON("a" << -1)));
    ASSERT(indexComp(pattern, BSON("a" << 1 << "b" << 1)));
    ASSERT(indexComp(pattern, BSON("a" << -1 << "b" << 1)));

    ASSERT(indexComp(pattern, BSON("_id" << 1)));
    ASSERT(indexComp(pattern, BSON("_id" << -1 << "b" << 1)));

    ASSERT(!indexComp(pattern, BSON("b" << 1)));
    ASSERT(!indexComp(pattern, BSON("b" << -1 << "a" << 1)));
}

TEST(ShardKeyPattern, UniqueIndexCompatibleCompound) {
    //
    // Compound ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1 << "b" << 1.0));
    ASSERT(indexComp(pattern, BSON("a" << 1 << "b" << 1)));
    ASSERT(indexComp(pattern, BSON("a" << 1 << "b" << -1.0)));
    ASSERT(indexComp(pattern, BSON("a" << 1 << "b" << -1.0 << "c" << 1)));

    ASSERT(indexComp(pattern, BSON("_id" << 1)));
    ASSERT(indexComp(pattern, BSON("_id" << -1 << "c" << 1)));

    ASSERT(!indexComp(pattern, BSON("a" << 1)));
    ASSERT(!indexComp(pattern, BSON("b" << 1)));
    ASSERT(!indexComp(pattern, BSON("a" << 1 << "c" << 1.0f)));
    ASSERT(!indexComp(pattern, BSON("b" << -1 << "a" << 1 << "c" << 1)));
}

TEST(ShardKeyPattern, UniqueIndexCompatibleNested) {
    //
    // Nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0));
    ASSERT(indexComp(pattern, BSON("a.b" << 1 << "c" << 1.0f)));

    ASSERT(!indexComp(pattern, BSON("a.b" << 1)));
    ASSERT(!indexComp(pattern, BSON("a" << 1 << "c" << -1.0)));
    ASSERT(!indexComp(pattern, BSON("c" << -1 << "a.b" << 1)));
}

TEST(ShardKeyPattern, UniqueIndexCompatibleHashed) {
    //
    // Hashed ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b"
                                 << "hashed"));

    ASSERT(indexComp(pattern, BSON("a.b" << 1)));
    ASSERT(indexComp(pattern, BSON("a.b" << -1)));
    ASSERT(indexComp(pattern, BSON("a.b" << 1 << "c" << 1)));
    ASSERT(indexComp(pattern, BSON("a.b" << -1 << "c" << 1)));

    ASSERT(indexComp(pattern,
                     BSON("a.b"
                          << "hashed")));

    ASSERT(indexComp(pattern, BSON("_id" << 1)));
    ASSERT(indexComp(pattern, BSON("_id" << -1 << "c" << 1)));

    ASSERT(!indexComp(pattern, BSON("c" << 1)));
    ASSERT(!indexComp(pattern, BSON("c" << -1 << "a.b" << 1)));
}

}  // namespace
}  // namespace mongo
