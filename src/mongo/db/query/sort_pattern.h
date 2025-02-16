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
#pragma once

#include <vector>

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {

class SortPattern {
public:
    enum class SortKeySerialization {
        kForExplain,
        kForPipelineSerialization,
        kForSortKeyMerging,
    };

    // Represents one of the components in a compound sort pattern. Each component is either the
    // field path by which we are sorting, or an Expression which can be used to retrieve the sort
    // value in the case of a $meta-sort (but not both).
    struct SortPatternPart {
        bool isAscending = true;
        boost::optional<FieldPath> fieldPath;
        boost::intrusive_ptr<Expression> expression;
    };

    SortPattern(const BSONObj&, const boost::intrusive_ptr<ExpressionContext>&);

    /**
     * Write out a Document whose contents are the sort key pattern.
     */
    Document serialize(SortKeySerialization) const;

    /**
     * Serializes the document to BSON, only keeping the paths specified in the sort pattern.
     */
    BSONObj documentToBsonWithSortPaths(const Document& doc) const {
        return document_path_support::documentToBsonWithPaths(doc, _paths);
    }

    size_t size() const {
        return _sortPattern.size();
    }

    bool empty() const {
        return _sortPattern.empty();
    }

    SortPatternPart operator[](int idx) const {
        return _sortPattern[idx];
    }

    std::vector<SortPatternPart>::const_iterator begin() const {
        return _sortPattern.cbegin();
    }

    std::vector<SortPatternPart>::const_iterator end() const {
        return _sortPattern.cend();
    }

private:
    std::vector<SortPatternPart> _sortPattern;

    // The set of paths on which we're sorting.
    std::set<std::string> _paths;
};
}  // namespace mongo
