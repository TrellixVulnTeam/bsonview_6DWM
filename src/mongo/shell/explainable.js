//
// A view of a collection against which operations are explained rather than executed
// normally.
//

var Explainable = (function() {

    var parseVerbosity = function(verbosity) {
        // Truthy non-strings are interpreted as "allPlansExecution" verbosity.
        if (verbosity && (typeof verbosity !== "string")) {
            return "allPlansExecution";
        }

        // Falsy non-strings are interpreted as "queryPlanner" verbosity.
        if (!verbosity && (typeof verbosity !== "string")) {
            return "queryPlanner";
        }

        // If we're here, then the verbosity is a string. We reject invalid strings.
        if (verbosity !== "queryPlanner" && verbosity !== "executionStats" &&
            verbosity !== "allPlansExecution") {
            throw Error("explain verbosity must be one of {" + "'queryPlanner'," +
                        "'executionStats'," + "'allPlansExecution'}");
        }

        return verbosity;
    };

    var throwOrReturn = function(explainResult) {
        if (("ok" in explainResult && !explainResult.ok) || explainResult.$err) {
            throw _getErrorWithCode(explainResult, "explain failed: " + tojson(explainResult));
        }

        return explainResult;
    };

    function constructor(collection, verbosity) {
        //
        // Private vars.
        //

        this._collection = collection;
        this._verbosity = parseVerbosity(verbosity);

        //
        // Public methods.
        //

        this.getCollection = function() {
            return this._collection;
        };

        this.getVerbosity = function() {
            return this._verbosity;
        };

        this.setVerbosity = function(verbosity) {
            this._verbosity = parseVerbosity(verbosity);
            return this;
        };

        this.help = function() {
            print("Explainable operations");
            print("\t.aggregate(...) - explain an aggregation operation");
            print("\t.count(...) - explain a count operation");
            print("\t.distinct(...) - explain a distinct operation");
            print("\t.find(...) - get an explainable query");
            print("\t.findAndModify(...) - explain a findAndModify operation");
            print("\t.remove(...) - explain a remove operation");
            print("\t.update(...) - explain an update operation");
            print("Explainable collection methods");
            print("\t.getCollection()");
            print("\t.getVerbosity()");
            print("\t.setVerbosity(verbosity)");
            return __magicNoPrint;
        };

        //
        // Pretty representations.
        //

        this.toString = function() {
            return "Explainable(" + this._collection.getFullName() + ")";
        };

        this.shellPrint = function() {
            return this.toString();
        };

        //
        // Explainable operations.
        //

        this.aggregate = function(pipeline, extraOpts) {
            if (!(pipeline instanceof Array)) {
                // Support legacy varargs form. (Also handles db.foo.aggregate())
                pipeline = Array.from(arguments);
                extraOpts = {};
            }

            // Add the explain option.
            let extraOptsCopy = Object.extend({}, (extraOpts || {}));

            // For compatibility with 3.4 and older versions, when the verbosity is "queryPlanner",
            // we use the explain option to the aggregate command. Otherwise we issue an explain
            // command wrapping the agg command, which is supported by newer versions of the server.
            if (this._verbosity === "queryPlanner") {
                extraOptsCopy.explain = true;
                return this._collection.aggregate(pipeline, extraOptsCopy);
            } else {
                // The aggregate command requires a cursor field.
                if (!extraOptsCopy.hasOwnProperty("cursor")) {
                    extraOptsCopy = Object.extend(extraOptsCopy, {cursor: {}});
                }

                let aggCmd = Object.extend(
                    {"aggregate": this._collection.getName(), "pipeline": pipeline}, extraOptsCopy);
                let explainCmd = {"explain": aggCmd, "verbosity": this._verbosity};
                let explainResult = this._collection.runReadCommand(explainCmd);
                return throwOrReturn(explainResult);
            }
        };

        this.count = function(query, options) {
            query = this.find(query);
            return QueryHelpers._applyCountOptions(query, options).count();
        };

        /**
         * .explain().find() and .find().explain() mean the same thing. In both cases, we use
         * the DBExplainQuery abstraction in order to construct the proper explain command to send
         * to the server.
         */
        this.find = function() {
            var cursor = this._collection.find.apply(this._collection, arguments);
            return new DBExplainQuery(cursor, this._verbosity);
        };

        this.findAndModify = function(params) {
            var famCmd = Object.extend({"findAndModify": this._collection.getName()}, params);
            var explainCmd = {"explain": famCmd, "verbosity": this._verbosity};
            var explainResult = this._collection.runReadCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.distinct = function(keyString, query, options) {
            var distinctCmd = {
                distinct: this._collection.getName(),
                key: keyString,
                query: query || {}
            };

            if (options && options.hasOwnProperty("collation")) {
                distinctCmd.collation = options.collation;
            }

            var explainCmd = {explain: distinctCmd, verbosity: this._verbosity};
            var explainResult = this._collection.runReadCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.remove = function() {
            var parsed = this._collection._parseRemove.apply(this._collection, arguments);
            var query = parsed.query;
            var justOne = parsed.justOne;
            var collation = parsed.collation;

            var bulk = this._collection.initializeOrderedBulkOp();
            var removeOp = bulk.find(query);

            if (collation) {
                removeOp.collation(collation);
            }

            if (justOne) {
                removeOp.removeOne();
            } else {
                removeOp.remove();
            }

            var explainCmd = bulk.convertToExplainCmd(this._verbosity);
            var explainResult = this._collection.runCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.update = function() {
            var parsed = this._collection._parseUpdate.apply(this._collection, arguments);
            var query = parsed.query;
            var updateSpec = parsed.updateSpec;
            var upsert = parsed.upsert;
            var multi = parsed.multi;
            var collation = parsed.collation;
            var arrayFilters = parsed.arrayFilters;
            var hint = parsed.hint;

            var bulk = this._collection.initializeOrderedBulkOp();
            var updateOp = bulk.find(query);

            if (hint) {
                updateOp.hint(hint);
            }

            if (upsert) {
                updateOp = updateOp.upsert();
            }

            if (collation) {
                updateOp.collation(collation);
            }

            if (arrayFilters) {
                updateOp.arrayFilters(arrayFilters);
            }

            if (multi) {
                updateOp.update(updateSpec);
            } else {
                updateOp.updateOne(updateSpec);
            }

            var explainCmd = bulk.convertToExplainCmd(this._verbosity);
            var explainResult = this._collection.runCommand(explainCmd);
            return throwOrReturn(explainResult);
        };
    }

    //
    // Public static methods.
    //

    constructor.parseVerbosity = parseVerbosity;
    constructor.throwOrReturn = throwOrReturn;

    return constructor;
})();

/**
 * This is the user-facing method for creating an Explainable from a collection.
 */
DBCollection.prototype.explain = function(verbosity) {
    return new Explainable(this, verbosity);
};
