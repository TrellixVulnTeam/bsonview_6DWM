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

#include <memory>

namespace mongo {
namespace logv2 {

class LogDomain;

/**
 * Container for managing log domains.
 *
 * Use this while setting up the logging system, before launching any threads.
 */
class LogManager {
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

public:
    LogManager();
    ~LogManager();

    static LogManager& global();

    /**
     * Gets the global domain for this manager.  It has no name.
     * Will attach a default console log appender.
     */
    LogDomain& getGlobalDomain();

    /**
    * Detaches the default log backends
    *
    * @note This function is not thread safe.
    */
    void detachDefaultBackends();

    /**
     * Reattaches the default log backends
     *
     * @note This function is not thread safe.
     */
    void reattachDefaultBackends();

    /**
    * Checks if the default log backends are attached
    */
    bool isDefaultBackendsAttached() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace logv2
}  // namespace mongo
