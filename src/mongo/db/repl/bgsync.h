/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/queue.h"

namespace mongo {

class DBClientBase;
class OperationContext;

namespace repl {

class ReplicationCoordinator;
class ReplicationCoordinatorExternalState;

/**
 * Lock order:
 * 1. rslock
 * 2. rwlock
 * 3. BackgroundSync::_mutex
 */
class BackgroundSync {
public:
    BackgroundSync();
    MONGO_DISALLOW_COPYING(BackgroundSync);

    // stop syncing (when this node becomes a primary, e.g.)
    void stop();

    void shutdown();

    bool isStopped() const;

    /**
     * Starts the producer thread which runs until shutdown. Upon resolving the current sync source
     * the producer thread uses the OplogFetcher (which requires the replication coordinator
     * external state at construction) to fetch oplog entries from the source's oplog via a long
     * running find query.
     */
    void producerThread(ReplicationCoordinatorExternalState* replicationCoordinatorExternalState);
    // starts the sync target notifying thread
    void notifierThread();

    HostAndPort getSyncTarget() const;

    // Interface implementation

    bool peek(BSONObj* op);
    void consume();
    void clearSyncTarget();
    void waitForMore();

    // For monitoring
    BSONObj getCounters();

    // Clears any fetched and buffered oplog entries.
    void clearBuffer();

    /**
     * Cancel existing find/getMore commands on the sync source's oplog collection.
     */
    void cancelFetcher();

    /**
     * Returns true if any of the following is true:
     * 1) We are shutting down;
     * 2) We are primary;
     * 3) We are in drain mode; or
     * 4) We are stopped.
     */
    bool shouldStopFetching() const;

    // Testing related stuff
    void pushTestOpToBuffer(const BSONObj& op);

private:
    // Production thread
    void _producerThread(ReplicationCoordinatorExternalState* replicationCoordinatorExternalState);
    void _produce(OperationContext* txn,
                  ReplicationCoordinatorExternalState* replicationCoordinatorExternalState);

    /**
     * Signals to the applier that we have no new data,
     * and are in sync with the applier at this point.
     *
     * NOTE: Used after rollback and during draining to transition to Primary role;
     */
    void _signalNoNewDataForApplier();

    /**
     * Record metrics.
     */
    void _recordStats(const OplogFetcher::DocumentsInfo& info, Milliseconds getMoreElapsedTime);

    /**
     * Checks current background sync state before pushing operations into blocking queue and
     * updating metrics. If the queue is full, might block.
     */
    void _enqueueDocuments(Fetcher::Documents::const_iterator begin,
                           Fetcher::Documents::const_iterator end,
                           const OplogFetcher::DocumentsInfo& info,
                           Milliseconds elapsed);

    /**
     * Executes a rollback.
     * 'getConnection' returns a connection to the sync source.
     */
    void _rollback(OperationContext* txn,
                   const HostAndPort& source,
                   stdx::function<DBClientBase*()> getConnection);

    // restart syncing
    void start(OperationContext* txn);

    long long _readLastAppliedHash(OperationContext* txn);

    // Production thread
    BlockingQueue<BSONObj> _buffer;

    // Task executor used to run find/getMore commands on sync source.
    executor::ThreadPoolTaskExecutor _threadPoolTaskExecutor;

    // A pointer to the replication coordinator running the show.
    ReplicationCoordinator* _replCoord;

    // Used to determine sync source.
    // TODO(dannenberg) move into DataReplicator.
    SyncSourceResolver _syncSourceResolver;

    // _mutex protects all of the class variables declared below.
    mutable stdx::mutex _mutex;

    OpTime _lastOpTimeFetched;

    // lastFetchedHash is used to match ops to determine if we need to rollback, when
    // a secondary.
    long long _lastFetchedHash = 0LL;

    // if producer thread should not be running
    bool _stopped = true;

    HostAndPort _syncSourceHost;

    // Current oplog fetcher tailing the oplog on the sync source.
    // Owned by us.
    std::unique_ptr<OplogFetcher> _oplogFetcher;
};


}  // namespace repl
}  // namespace mongo
