//
// LiveQuerier.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "LiveQuerier.hh"
#include "BackgroundDB.hh"
#include "DataFile.hh"
#include "DatabaseImpl.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include <inttypes.h>

namespace litecore {
    using namespace actor;
    using namespace std::placeholders;
    using namespace std;


    // Threshold for rapidity of database changes. If it's been this long since the last change,
    // we re-query after the short delay. Otherwise we use the long delay. This allows for very
    // low latency if changes are not too rapid, while also not flooding the app with notifications
    // if changes are rapid.
    static constexpr delay_t kRapidChanges = 250ms;

    static constexpr delay_t kShortDelay   = chrono::milliseconds(  0);
    static constexpr delay_t kLongDelay    = 500ms;


    LiveQuerier::LiveQuerier(DatabaseImpl *db,
                             Query *query,
                             bool continuous,
                             Delegate *delegate)
    :Actor(QueryLog)
    ,_database(db)
    ,_backgroundDB(db->backgroundDatabase())
    ,_expression(query->expression())
    ,_language(query->language())
    ,_continuous(continuous)
    ,_delegate(delegate)
    {
        logInfo("Created on Query %s", query->loggingName().c_str());
        // Note that we don't keep a reference to `_query`, because it's tied to `db`, but we
        // need to run the query on `_backgroundDB`. So instead we save the query text and
        // language, and create a new Query instance the first time `_runQuery` is called.
    }


    LiveQuerier::~LiveQuerier() {
        if (_query)
            _stop();
        logVerbose("Deleted");
    }


    std::string LiveQuerier::loggingIdentifier() const {
        return string(_expression);
    }


    void LiveQuerier::start(const Query::Options &options) {
        _lastTime = clock::now();
        _stopping = false;
        enqueue(FUNCTION_TO_QUEUE(LiveQuerier::_runQuery), options);
    }


    void LiveQuerier::stop() {
        logInfo("Stopping");
         _backgroundDB->dataFile().useLocked([&](DataFile *df) {
             // CBL-2335: Guard access to the _stopping variable so that
             // it is not changed at unpredictable times
             _stopping = true;
         });
        enqueue(FUNCTION_TO_QUEUE(LiveQuerier::_stop));
    }


    // Database change (transaction committed) notification
    void LiveQuerier::transactionCommitted() {
        enqueue(FUNCTION_TO_QUEUE(LiveQuerier::_dbChanged), clock::now());
    }


#pragma mark - ACTOR METHODS (single-threaded):


    void LiveQuerier::_stop() {
        if (_query) {
            _backgroundDB->dataFile().useLocked([&](DataFile *df) {
                _query = nullptr;
                _currentEnumerator = nullptr;
                if (_continuous)
                    _backgroundDB->removeTransactionObserver(this);
            });
        }
        logVerbose("...stopped");
    }


    void LiveQuerier::_dbChanged(clock::time_point when) {
        // Do nothing if there's already a _runQuery call pending (but not yet running),
        // or I've already been told to stop, or the query can't be run:
        if (_waitingToRun || _stopping || !_currentEnumerator)
            return;

        delay_t idleTime = when - _lastTime;
        _lastTime = when;

        delay_t delay = (idleTime <= kRapidChanges) ? kLongDelay : kShortDelay;
        logVerbose("DB changed after %.3f sec. Triggering query in %.3f secs",
                   idleTime.count(), delay.count());
        enqueueAfter(delay, FUNCTION_TO_QUEUE(LiveQuerier::_runQuery), _currentEnumerator->options());
        _waitingToRun = true;
    }


    void LiveQuerier::_runQuery(Query::Options options) {
        _waitingToRun = false;
        logVerbose("Running query...");
        Retained<QueryEnumerator> newQE;
        C4Error error = {};
        fleece::Stopwatch st;
        _backgroundDB->dataFile().useLocked([&](DataFile *df) {
            if (_stopping) {
                // CBL-2335: Guard access to the _stopping variable so that
                // it is not changed at unpredictable times
                return;
            }

            try {
                if (_usuallyFalse(!df)) {
                    // CBL-2335: Backup for the above, to avoid a crash
                    C4Error::raise(LiteCoreDomain, kC4ErrorNotOpen);
                }

                // Create my own Query object associated with the Backgrounder's DataFile:
                if (!_query) {
                    _query = df->compileQuery(_expression, _language);
                    if (_continuous)
                        _backgroundDB->addTransactionObserver(this);
                }
                // Now run the query:
                newQE = _query->createEnumerator(&options);
            } catchError(&error);
        });
        auto time = st.elapsedMS();

        if (!newQE)
            logError("Query failed with error %s", error.description().c_str());

        if (_continuous) {
            if (newQE) {
                if (_currentEnumerator && !_currentEnumerator->obsoletedBy(newQE)) {
                    logVerbose("Results unchanged at seq %" PRIu64 " (%.3fms)",
                               newQE->lastSequence(), time);
                    return; // no delegate call
                }
                logInfo("Results changed at seq %" PRIu64 " (%.3fms)", newQE->lastSequence(), time);
                _currentEnumerator = newQE;
            }
        } else {
            logInfo("...finished one-shot query in %.3fms", time);
        }

        if (_stopping)
            return;
        
        _delegate->liveQuerierUpdated(newQE, error);
    }

}
