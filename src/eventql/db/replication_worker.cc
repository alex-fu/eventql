/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Paul Asmuth <paul@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include "unistd.h"
#include "eventql/eventql.h"
#include <eventql/util/logging.h>
#include <eventql/util/wallclock.h>
#include <eventql/util/application.h>
#include <eventql/db/replication_worker.h>
#include <eventql/db/partition.h>
#include <eventql/server/server_stats.h>
#include <assert.h>

namespace eventql {

ReplicationInfo::ReplicationInfo() {
  reset();
}

void ReplicationInfo::reset() {
  std::unique_lock<std::mutex> lk(mutex_);
  is_idle_ = true;
  cur_partition_.clear();
  cur_target_host_.clear();
  cur_partition_since_ = UnixTime(0);
  cur_target_host_since_ = UnixTime(0);
  cur_target_host_bytes_sent_ = 0;
  cur_target_host_records_sent_ = 0;
}

void ReplicationInfo::setPartition(String name) {
  std::unique_lock<std::mutex> lk(mutex_);
  is_idle_ = false;
  cur_partition_ = name;
  cur_partition_since_ = WallClock::now();
}

void ReplicationInfo::setTargetHost(String host_name) {
  std::unique_lock<std::mutex> lk(mutex_);
  cur_target_host_ = host_name;
  cur_target_host_since_ = WallClock::now();
  cur_target_host_bytes_sent_ = 0;
  cur_target_host_records_sent_ = 0;
}

void ReplicationInfo::setTargetHostStatus(
    size_t bytes_sent,
    size_t records_sent) {
  cur_target_host_bytes_sent_ = bytes_sent;
  cur_target_host_records_sent_ = records_sent;
}

String ReplicationInfo::toString() const {
  std::unique_lock<std::mutex> lk(mutex_);
  if (is_idle_) {
    return "Idle";
  }

  if (cur_target_host_.empty()) {
    return StringUtil::format(
        "Replicating partition $0 (since $1s)",
        cur_partition_,
        (WallClock::unixMicros() - cur_partition_since_.unixMicros()) / kMicrosPerSecond);

  } else {
    auto duration = (WallClock::unixMicros() - cur_target_host_since_.unixMicros()) / kMicrosPerSecond;
    return StringUtil::format(
        "Replicating partition $0 (since $1s) to host $2 (since $3s); records_sent=$4 bytes_sent=$5MB bw=$6kb/s",
        cur_partition_,
        (WallClock::unixMicros() - cur_partition_since_.unixMicros()) / kMicrosPerSecond,
        cur_target_host_,
        duration,
        cur_target_host_records_sent_,
        cur_target_host_bytes_sent_ / double(1024 * 1024),
        (cur_target_host_bytes_sent_ / (duration < 1 ? 1 : duration)) / 1024);
  }
}

ReplicationWorker::ReplicationWorker(
    PartitionMap* pmap,
    size_t replication_threads_max) :
    pmap_(pmap),
    queue_([] (
        const Pair<uint64_t, RefPtr<Partition>>& a,
        const Pair<uint64_t, RefPtr<Partition>>& b) {
      return a.first < b.first;
    }),
    running_(false),
    num_replication_threads_(replication_threads_max),
    replication_infos_(num_replication_threads_) {
  pmap->subscribeToPartitionChanges([this] (
      RefPtr<eventql::PartitionChangeNotification> change) {
    enqueuePartition(change->partition);
  });
}

ReplicationWorker::~ReplicationWorker() {
  assert(running_ == false);
}

void ReplicationWorker::enqueuePartition(
    RefPtr<Partition> partition,
    uint64_t flags /* = ReplicationOptions::CORK */) {
  std::unique_lock<std::mutex> lk(mutex_);
  enqueuePartitionWithLock(partition, flags);
}

void ReplicationWorker::enqueuePartitionWithLock(
    RefPtr<Partition> partition,
    uint64_t flags) {
  if (partition->getTable()->config().config().disable_replication()) {
    return;
  }

  if (partition->isSplitting()) {
    flags &= ~(uint64_t) ReplicationOptions::CORK;
    flags |= (uint64_t) ReplicationOptions::URGENT;
  }

  uint64_t enqueue_at = WallClock::unixMicros();
  if (flags & (uint64_t) ReplicationOptions::CORK) {
    enqueue_at += kReplicationCorkWindowMicros;
  }

  if (flags & (uint64_t) ReplicationOptions::URGENT) {
    enqueue_at = 0;
  }

  auto uuid = partition->uuid();
  if (waitset_.count(uuid) > 0) {
    return;
  }

  queue_.emplace(enqueue_at, partition);
  waitset_.emplace(uuid);
  cv_.notify_all();

  evqld_stats()->replication_queue_length.set(queue_.size());
}

void ReplicationWorker::start() {
  running_ = true;

  for (int i = 0; i < num_replication_threads_; ++i) {
    threads_.emplace_back(std::bind(&ReplicationWorker::work, this, i));
  }
}

void ReplicationWorker::stop() {
  if (!running_) {
    return;
  }

  running_ = false;
  cv_.notify_all();

  for (auto& t : threads_) {
    t.join();
  }
}

void ReplicationWorker::work(size_t thread_id) {
  Application::setCurrentThreadName("evqld-replication");
  auto replication_info = &replication_infos_[thread_id];

  std::unique_lock<std::mutex> lk(mutex_);

  while (running_) {
    if (queue_.size() == 0) {
      cv_.wait(lk);
    }

    if (queue_.size() == 0) {
      continue;
    }

    auto now = WallClock::unixMicros();
    if (now < queue_.begin()->first) {
      cv_.wait_for(
          lk,
          std::chrono::microseconds(queue_.begin()->first - now));

      continue;
    }

    auto partition = queue_.begin()->second;
    queue_.erase(queue_.begin());

    RefPtr<PartitionReplication> repl;
    bool success = true;
    {
      lk.unlock();

      auto snap = partition->getSnapshot();
      replication_info->setPartition(StringUtil::format(
          "$0/$1/$2",
          snap->state.tsdb_namespace(),
          snap->state.table_key(),
          snap->key));

      try {
        repl = partition->getReplicationStrategy();
        success = repl->replicate(replication_info);
      } catch (const StandardException& e) {
        logError("evqld", e, "ReplicationWorker error");
        success = false;
      }

      replication_info->reset();
      lk.lock();
    }

    if (success) {
      waitset_.erase(partition->uuid());

      repl = partition->getReplicationStrategy();
      if (repl->needsReplication()) {
        enqueuePartitionWithLock(
            partition,
            (uint64_t) ReplicationOptions::CORK);
      } else {
        repl = partition->getReplicationStrategy();
        if (repl->shouldDropPartition()) {
          auto snap = partition->getSnapshot();
          auto dropped =
              pmap_->dropLocalPartition(
                  snap->state.tsdb_namespace(),
                  snap->state.table_key(),
                  snap->key);

          if (!dropped) {
            enqueuePartitionWithLock(
                partition,
                (uint64_t) ReplicationOptions::CORK);
          }
        }
      }
    } else {
      auto delay = 30 * kMicrosPerSecond; // FIXME increasing delay..
      queue_.emplace(now + delay, partition);
    }

    evqld_stats()->replication_queue_length.set(queue_.size());
  }
}

size_t ReplicationWorker::getNumThreads() const {
  return num_replication_threads_;
}

const ReplicationInfo* ReplicationWorker::getReplicationInfo(
    size_t thread_id) const {
  if (thread_id >= num_replication_threads_) {
    return nullptr;
  }

  return &replication_infos_[thread_id];
}


} // namespace eventql
