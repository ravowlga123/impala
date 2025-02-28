// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/partitioned-hash-join-builder.h"

#include <numeric>

#include <gutil/strings/substitute.h>

#include "codegen/llvm-codegen.h"
#include "exec/hash-table.inline.h"
#include "exprs/scalar-expr.h"
#include "exprs/scalar-expr-evaluator.h"
#include "runtime/buffered-tuple-stream.h"
#include "runtime/exec-env.h"
#include "runtime/mem-tracker.h"
#include "runtime/query-state.h"
#include "runtime/row-batch.h"
#include "runtime/runtime-filter-bank.h"
#include "runtime/runtime-filter.h"
#include "runtime/runtime-state.h"
#include "util/bloom-filter.h"
#include "util/min-max-filter.h"
#include "util/runtime-profile-counters.h"

#include "gen-cpp/PlanNodes_types.h"

#include "common/names.h"

static const string PREPARE_FOR_READ_FAILED_ERROR_MSG =
    "Failed to acquire initial read "
    "buffer for stream in hash join node $0. Reducing query concurrency or increasing "
    "the memory limit may help this query to complete successfully.";

using namespace impala;
using strings::Substitute;

const char* PhjBuilder::LLVM_CLASS_NAME = "class.impala::PhjBuilder";

PhjBuilder::PhjBuilder(int join_node_id, const string& join_node_label,
    TJoinOp::type join_op, const RowDescriptor* build_row_desc, RuntimeState* state,
    BufferPool::ClientHandle* buffer_pool_client, int64_t spillable_buffer_size,
    int64_t max_row_buffer_size)
  : DataSink(-1, build_row_desc,
        Substitute("Hash Join Builder (join_node_id=$0)", join_node_id), state),
    runtime_state_(state),
    join_node_id_(join_node_id),
    join_node_label_(join_node_label),
    join_op_(join_op),
    buffer_pool_client_(buffer_pool_client),
    spillable_buffer_size_(spillable_buffer_size),
    max_row_buffer_size_(max_row_buffer_size) {}

Status PhjBuilder::InitExprsAndFilters(RuntimeState* state,
    const vector<TEqJoinCondition>& eq_join_conjuncts,
    const vector<TRuntimeFilterDesc>& filter_descs) {
  for (const TEqJoinCondition& eq_join_conjunct : eq_join_conjuncts) {
    ScalarExpr* build_expr;
    RETURN_IF_ERROR(
        ScalarExpr::Create(eq_join_conjunct.right, *row_desc_, state, &build_expr));
    build_exprs_.push_back(build_expr);
    is_not_distinct_from_.push_back(eq_join_conjunct.is_not_distinct_from);
  }

  for (const TRuntimeFilterDesc& filter_desc : filter_descs) {
    DCHECK(state->query_options().runtime_filter_mode == TRuntimeFilterMode::GLOBAL ||
        filter_desc.is_broadcast_join || state->query_options().num_nodes == 1);
    DCHECK(!state->query_options().disable_row_runtime_filtering ||
        filter_desc.applied_on_partition_columns);
    // Skip over filters that are not produced by this instance of the join, i.e.
    // broadcast filters where this instance was not selected as a filter producer.
    const vector<TRuntimeFilterSource> filters_produced =
        state->instance_ctx().filters_produced;
    auto it = std::find_if(filters_produced.begin(), filters_produced.end(),
        [this, &filter_desc](const TRuntimeFilterSource f) {
          return f.src_node_id == join_node_id_ && f.filter_id == filter_desc.filter_id;
        });
    if (it == filters_produced.end()) continue;
    ScalarExpr* filter_expr;
    RETURN_IF_ERROR(
        ScalarExpr::Create(filter_desc.src_expr, *row_desc_, state, &filter_expr));
    filter_exprs_.push_back(filter_expr);

    // TODO: Move to Prepare().
    filter_ctxs_.emplace_back();
    // TODO: IMPALA-4400 - implement local aggregation of runtime filters.
    filter_ctxs_.back().filter = state->filter_bank()->RegisterFilter(filter_desc, true);
  }
  return Status::OK();
}

Status PhjBuilder::Prepare(RuntimeState* state, MemTracker* parent_mem_tracker) {
  RETURN_IF_ERROR(DataSink::Prepare(state, parent_mem_tracker));
  RETURN_IF_ERROR(HashTableCtx::Create(&obj_pool_, state, build_exprs_, build_exprs_,
      HashTableStoresNulls(), is_not_distinct_from_, state->fragment_hash_seed(),
      MAX_PARTITION_DEPTH, row_desc_->tuple_descriptors().size(), expr_perm_pool_.get(),
      expr_results_pool_.get(), expr_results_pool_.get(), &ht_ctx_));

  DCHECK_EQ(filter_exprs_.size(), filter_ctxs_.size());
  for (int i = 0; i < filter_exprs_.size(); ++i) {
    RETURN_IF_ERROR(ScalarExprEvaluator::Create(*filter_exprs_[i], state, &obj_pool_,
        expr_perm_pool_.get(), expr_results_pool_.get(), &filter_ctxs_[i].expr_eval));
  }

  partitions_created_ = ADD_COUNTER(profile(), "PartitionsCreated", TUnit::UNIT);
  largest_partition_percent_ =
      profile()->AddHighWaterMarkCounter("LargestPartitionPercent", TUnit::UNIT);
  max_partition_level_ =
      profile()->AddHighWaterMarkCounter("MaxPartitionLevel", TUnit::UNIT);
  num_build_rows_partitioned_ =
      ADD_COUNTER(profile(), "BuildRowsPartitioned", TUnit::UNIT);
  ht_stats_profile_ = HashTable::AddHashTableCounters(profile());
  num_spilled_partitions_ = ADD_COUNTER(profile(), "SpilledPartitions", TUnit::UNIT);
  num_repartitions_ = ADD_COUNTER(profile(), "NumRepartitions", TUnit::UNIT);
  partition_build_rows_timer_ = ADD_TIMER(profile(), "BuildRowsPartitionTime");
  build_hash_table_timer_ = ADD_TIMER(profile(), "HashTablesBuildTime");
  num_hash_table_builds_skipped_ =
      ADD_COUNTER(profile(), "NumHashTableBuildsSkipped", TUnit::UNIT);
  repartition_timer_ = ADD_TIMER(profile(), "RepartitionTime");
  state->CheckAndAddCodegenDisabledMessage(profile());
  return Status::OK();
}

Status PhjBuilder::Open(RuntimeState* state) {
  // Need to init here instead of constructor so that buffer_pool_client_ is registered.
  if (probe_stream_reservation_.is_closed()) {
    probe_stream_reservation_.Init(buffer_pool_client_);
  }

  RETURN_IF_ERROR(ht_ctx_->Open(state));

  for (const FilterContext& ctx : filter_ctxs_) {
    RETURN_IF_ERROR(ctx.expr_eval->Open(state));
  }
  if (ht_allocator_ == nullptr) {
    // Create 'ht_allocator_' on the first call to Open().
    ht_allocator_.reset(new Suballocator(ExecEnv::GetInstance()->buffer_pool(),
        buffer_pool_client_, spillable_buffer_size_));
  }
  RETURN_IF_ERROR(CreateHashPartitions(0));
  AllocateRuntimeFilters();

  if (join_op_ == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN) {
    RETURN_IF_ERROR(CreateAndPreparePartition(0, &null_aware_partition_));
  }
  return Status::OK();
}

Status PhjBuilder::Send(RuntimeState* state, RowBatch* batch) {
  SCOPED_TIMER(partition_build_rows_timer_);
  bool build_filters = ht_ctx_->level() == 0 && filter_ctxs_.size() > 0;
  if (process_build_batch_fn_ == nullptr) {
    RETURN_IF_ERROR(ProcessBuildBatch(batch, ht_ctx_.get(), build_filters,
        join_op_ == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN));

  } else {
    DCHECK(process_build_batch_fn_level0_ != nullptr);
    if (ht_ctx_->level() == 0) {
      RETURN_IF_ERROR(
          process_build_batch_fn_level0_(this, batch, ht_ctx_.get(), build_filters,
              join_op_ == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN));
    } else {
      RETURN_IF_ERROR(process_build_batch_fn_(this, batch, ht_ctx_.get(), build_filters,
          join_op_ == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN));
    }
  }

  // Free any expr result allocations made during partitioning.
  expr_results_pool_->Clear();
  COUNTER_ADD(num_build_rows_partitioned_, batch->num_rows());
  return Status::OK();
}

Status PhjBuilder::FlushFinal(RuntimeState* state) {
  int64_t num_build_rows = 0;
  for (Partition* partition : hash_partitions_) {
    num_build_rows += partition->build_rows()->num_rows();
  }

  if (num_build_rows > 0) {
    double largest_fraction = 0.0;
    for (Partition* partition : hash_partitions_) {
      largest_fraction = max(largest_fraction,
          partition->build_rows()->num_rows() / static_cast<double>(num_build_rows));
    }
    COUNTER_SET(largest_partition_percent_, static_cast<int64_t>(largest_fraction * 100));
  }

  if (VLOG_IS_ON(2)) {
    stringstream ss;
    ss << Substitute("PHJ(node_id=$0) partitioned(level=$1) $2 rows into:", join_node_id_,
        hash_partitions_[0]->level(), num_build_rows);
    for (int i = 0; i < hash_partitions_.size(); ++i) {
      Partition* partition = hash_partitions_[i];
      double percent = num_build_rows == 0 ? 0.0 : partition->build_rows()->num_rows()
              * 100 / static_cast<double>(num_build_rows);
      ss << "  " << i << " " << (partition->is_spilled() ? "spilled" : "not spilled")
         << " (fraction=" << fixed << setprecision(2) << percent << "%)" << endl
         << "    #rows:" << partition->build_rows()->num_rows() << endl;
    }
    if (null_aware_partition_ != nullptr) {
      ss << " Null-aware partition: " << null_aware_partition_->DebugString();
    }
    VLOG(2) << ss.str();
  }

  if (ht_ctx_->level() == 0) {
    PublishRuntimeFilters(num_build_rows);
    non_empty_build_ |= (num_build_rows > 0);
  }

  if (null_aware_partition_ != nullptr && null_aware_partition_->is_spilled()) {
    // Free up memory for the hash tables of other partitions by unpinning the
    // last block of the null aware partition's stream.
    RETURN_IF_ERROR(null_aware_partition_->Spill(BufferedTupleStream::UNPIN_ALL));
  }

  RETURN_IF_ERROR(BuildHashTablesAndReserveProbeBuffers());
  if (state_ == HashJoinState::PARTITIONING_BUILD) {
    UpdateState(HashJoinState::PARTITIONING_PROBE);
  } else {
    DCHECK_ENUM_EQ(state_, HashJoinState::REPARTITIONING_BUILD);
    UpdateState(HashJoinState::REPARTITIONING_PROBE);
  }
  return Status::OK();
}

void PhjBuilder::Close(RuntimeState* state) {
  if (closed_) return;
  CloseAndDeletePartitions(nullptr);
  if (ht_ctx_ != nullptr) ht_ctx_->Close(state);
  ht_ctx_.reset();
  for (const FilterContext& ctx : filter_ctxs_) {
    if (ctx.expr_eval != nullptr) ctx.expr_eval->Close(state);
  }
  ScalarExpr::Close(filter_exprs_);
  ScalarExpr::Close(build_exprs_);
  obj_pool_.Clear();
  probe_stream_reservation_.Close();
  DataSink::Close(state);
  closed_ = true;
}

void PhjBuilder::Reset(RowBatch* row_batch) {
  DCHECK_EQ(0, probe_stream_reservation_.GetReservation());
  state_ = HashJoinState::PARTITIONING_BUILD;
  expr_results_pool_->Clear();
  non_empty_build_ = false;
  CloseAndDeletePartitions(row_batch);
}

void PhjBuilder::UpdateState(HashJoinState next_state) {
  // Validate the state transition.
  switch (state_) {
    case HashJoinState::PARTITIONING_BUILD:
      DCHECK_ENUM_EQ(next_state, HashJoinState::PARTITIONING_PROBE);
      break;
    case HashJoinState::PARTITIONING_PROBE:
    case HashJoinState::REPARTITIONING_PROBE:
    case HashJoinState::PROBING_SPILLED_PARTITION:
      DCHECK(next_state == HashJoinState::REPARTITIONING_BUILD
          || next_state == HashJoinState::PROBING_SPILLED_PARTITION);
      break;
    case HashJoinState::REPARTITIONING_BUILD:
      DCHECK_ENUM_EQ(next_state, HashJoinState::REPARTITIONING_PROBE);
      break;
    default:
      DCHECK(false) << "Invalid state " << static_cast<int>(state_);
  }
  state_ = next_state;
  VLOG(2) << "Transitioned State:" << endl << DebugString();
}

string PhjBuilder::PrintState() const {
  switch (state_) {
    case HashJoinState::PARTITIONING_BUILD:
      return "PartitioningBuild";
    case HashJoinState::PARTITIONING_PROBE:
      return "PartitioningProbe";
    case HashJoinState::PROBING_SPILLED_PARTITION:
      return "ProbingSpilledPartition";
    case HashJoinState::REPARTITIONING_BUILD:
      return "RepartitioningBuild";
    case HashJoinState::REPARTITIONING_PROBE:
      return "RepartitioningProbe";
    default:
      DCHECK(false);
  }
  return "";
}

Status PhjBuilder::CreateAndPreparePartition(int level, Partition** partition) {
  all_partitions_.emplace_back(new Partition(runtime_state_, this, level));
  *partition = all_partitions_.back().get();
  RETURN_IF_ERROR((*partition)->build_rows()->Init(join_node_label_, true));
  bool got_buffer;
  RETURN_IF_ERROR((*partition)->build_rows()->PrepareForWrite(&got_buffer));
  DCHECK(got_buffer)
      << "Accounted in min reservation" << buffer_pool_client_->DebugString();
  return Status::OK();
}

Status PhjBuilder::CreateHashPartitions(int level) {
  DCHECK(hash_partitions_.empty());
  ht_ctx_->set_level(level); // Set the hash function for partitioning input.
  for (int i = 0; i < PARTITION_FANOUT; ++i) {
    Partition* new_partition;
    RETURN_IF_ERROR(CreateAndPreparePartition(level, &new_partition));
    hash_partitions_.push_back(new_partition);
  }
  COUNTER_ADD(partitions_created_, PARTITION_FANOUT);
  COUNTER_SET(max_partition_level_, level);
  return Status::OK();
}

bool PhjBuilder::AppendRowStreamFull(
    BufferedTupleStream* stream, TupleRow* row, Status* status) noexcept {
  while (true) {
    // We ran out of memory. Pick a partition to spill. If we ran out of unspilled
    // partitions, SpillPartition() will return an error status.
    *status = SpillPartition(BufferedTupleStream::UNPIN_ALL_EXCEPT_CURRENT);
    if (!status->ok()) return false;
    if (stream->AddRow(row, status)) return true;
    if (!status->ok()) return false;
    // Spilling one partition does not guarantee we can append a row. Keep
    // spilling until we can append this row.
  }
}

// TODO: can we do better with a different spilling heuristic?
Status PhjBuilder::SpillPartition(BufferedTupleStream::UnpinMode mode,
    Partition** spilled_partition) {
  DCHECK_EQ(hash_partitions_.size(), PARTITION_FANOUT);
  Partition* best_candidate = nullptr;
  if (null_aware_partition_ != nullptr && null_aware_partition_->CanSpill()) {
    // Spill null-aware partition first if possible - it is always processed last.
    best_candidate = null_aware_partition_;
  } else {
    // Iterate over the partitions and pick the largest partition to spill.
    int64_t max_freed_mem = 0;
    for (Partition* candidate : hash_partitions_) {
      if (!candidate->CanSpill()) continue;
      int64_t mem = candidate->build_rows()->BytesPinned(false);
      if (candidate->hash_tbl() != nullptr) {
        // The hash table should not have matches, since we have not probed it yet.
        // Losing match info would lead to incorrect results (IMPALA-1488).
        DCHECK(!candidate->hash_tbl()->HasMatches());
        mem += candidate->hash_tbl()->ByteSize();
      }
      if (mem > max_freed_mem) {
        max_freed_mem = mem;
        best_candidate = candidate;
      }
    }
  }

  if (best_candidate == nullptr) {
    return Status(Substitute("Internal error: could not find a partition to spill in "
                             " hash join $0: \n$1\nClient:\n$2",
        join_node_id_, DebugString(), buffer_pool_client_->DebugString()));
  }

  VLOG(2) << "Spilling partition: " << best_candidate->DebugString() << endl
          << DebugString();
  RETURN_IF_ERROR(best_candidate->Spill(mode));
  if (spilled_partition != nullptr) *spilled_partition = best_candidate;
  return Status::OK();
}

// When this function is called, we've finished processing the current build input
// (either from the child ExecNode or from repartitioning a spilled partition). The build
// rows have only been partitioned, we still need to build hash tables over them. Some
// of the partitions could have already been spilled and attempting to build hash
// tables over the non-spilled ones can cause them to spill.
//
// At the end of the function all partitions either have a hash table (and therefore are
// not spilled) or are spilled. Partitions that have hash tables don't need to spill on
// the probe side.
//
// This maps perfectly to a 0-1 knapsack where the weight is the memory to keep the
// build rows and hash table and the value is the expected IO savings.
// For now, we go with a greedy solution.
//
// TODO: implement the knapsack solution.
Status PhjBuilder::BuildHashTablesAndReserveProbeBuffers() {
  DCHECK_EQ(PARTITION_FANOUT, hash_partitions_.size());

  for (int i = 0; i < PARTITION_FANOUT; ++i) {
    Partition* partition = hash_partitions_[i];
    if (partition->build_rows()->num_rows() == 0) {
      // This partition is empty, no need to do anything else.
      partition->Close(nullptr);
    } else if (partition->is_spilled()) {
      // We don't need any build-side data for spilled partitions in memory.
      RETURN_IF_ERROR(
          partition->build_rows()->UnpinStream(BufferedTupleStream::UNPIN_ALL));
    }
  }

  // TODO: the below logic could be improved calculating upfront how much memory is needed
  // for each hash table, and only building hash tables that will eventually fit in
  // memory. In some cases now we could build a hash table, then spill the partition
  // later.

  // Allocate probe buffers for all partitions that are already spilled. Do this before
  // building hash tables because allocating probe buffers may cause some more partitions
  // to be spilled. This avoids wasted work on building hash tables for partitions that
  // won't fit in memory alongside the required probe buffers.
  bool input_is_spilled = ht_ctx_->level() > 0;
  RETURN_IF_ERROR(ReserveProbeBuffers(input_is_spilled));

  for (int i = 0; i < PARTITION_FANOUT; ++i) {
    Partition* partition = hash_partitions_[i];
    if (partition->IsClosed() || partition->is_spilled()) continue;

    bool built = false;
    DCHECK(partition->build_rows()->is_pinned());
    RETURN_IF_ERROR(partition->BuildHashTable(&built));
    // If we did not have enough memory to build this hash table, we need to spill this
    // partition (clean up the hash table, unpin build).
    if (!built) RETURN_IF_ERROR(partition->Spill(BufferedTupleStream::UNPIN_ALL));
  }
  // We may have spilled additional partitions while building hash tables, we need to
  // reserve memory for the probe buffers for those additional spilled partitions.
  RETURN_IF_ERROR(ReserveProbeBuffers(input_is_spilled));
  return Status::OK();
}

Status PhjBuilder::ReserveProbeBuffers(bool input_is_spilled) {
  DCHECK_EQ(PARTITION_FANOUT, hash_partitions_.size());

  // We need a write buffer for probe rows for each spilled partition, and a read buffer
  // if the input is a spilled partition (i.e. that we are repartitioning the input).
  int num_probe_streams =
      GetNumSpilledPartitions(hash_partitions_) + (input_is_spilled ? 1 : 0);
  int64_t per_stream_reservation = spillable_buffer_size_;
  int64_t addtl_reservation = num_probe_streams * per_stream_reservation
      - probe_stream_reservation_.GetReservation();

  // Loop until either we get enough reservation or all partitions are spilled (in which
  // case SpillPartition() returns an error).
  while (addtl_reservation > buffer_pool_client_->GetUnusedReservation()) {
    Partition* spilled_partition;
    RETURN_IF_ERROR(SpillPartition(
          BufferedTupleStream::UNPIN_ALL, &spilled_partition));
    // Don't need to create a probe stream for the null-aware partition.
    if (spilled_partition != null_aware_partition_) {
      addtl_reservation += per_stream_reservation;
    }
  }
  buffer_pool_client_->SaveReservation(&probe_stream_reservation_, addtl_reservation);
  return Status::OK();
}

PhjBuilder::HashPartitions PhjBuilder::BeginInitialProbe(
    BufferPool::ClientHandle* probe_client) {
  DCHECK_ENUM_EQ(state_, HashJoinState::PARTITIONING_PROBE);
  DCHECK_EQ(PARTITION_FANOUT, hash_partitions_.size());
  TransferProbeStreamReservation(probe_client);
  return HashPartitions(ht_ctx_->level(), &hash_partitions_, non_empty_build_);
}

void PhjBuilder::TransferProbeStreamReservation(BufferPool::ClientHandle* probe_client) {
  // An extra buffer is needed for reading spilled input stream, unless we're doing the
  // initial partitioning of rows from the left child.
  int num_buffers = GetNumSpilledPartitions(hash_partitions_)
      + (state_ == HashJoinState::PARTITIONING_PROBE ? 0 : 1);
  int64_t saved_reservation = probe_stream_reservation_.GetReservation();
  DCHECK_GE(saved_reservation, spillable_buffer_size_ * num_buffers);

  // TODO: in future we may need to support different clients for the probe.
  DCHECK_EQ(probe_client, buffer_pool_client_);
  probe_client->RestoreReservation(&probe_stream_reservation_, saved_reservation);
}

int PhjBuilder::GetNumSpilledPartitions(const vector<Partition*>& partitions) {
  int num_spilled = 0;
  for (int i = 0; i < partitions.size(); ++i) {
    Partition* partition = partitions[i];
    DCHECK(partition != nullptr);
    if (!partition->IsClosed() && partition->is_spilled()) ++num_spilled;
  }
  return num_spilled;
}

void PhjBuilder::DoneProbingHashPartitions(const bool retain_partition[PARTITION_FANOUT],
    list<Partition*>* output_partitions, RowBatch* batch) {
  DCHECK(output_partitions->empty());
  for (int i = 0; i < PARTITION_FANOUT; ++i) {
    PhjBuilder::Partition* partition = hash_partitions_[i];
    if (partition->IsClosed()) continue;
    if (partition->is_spilled()) {
      DCHECK(partition->hash_tbl() == nullptr) << DebugString();
      DCHECK_EQ(partition->build_rows()->BytesPinned(false), 0)
          << "Build was fully unpinned in BuildHashTablesAndPrepareProbeStreams()";
      // Release resources associated with completed partitions.
      if (!retain_partition[i]) {
        COUNTER_ADD(num_hash_table_builds_skipped_, 1);
        partition->Close(nullptr);
      }
    } else if (NeedToProcessUnmatchedBuildRows(join_op_)) {
      output_partitions->push_back(partition);
    } else {
      // No more processing is required for this partition.
      partition->Close(batch);
    }
  }
  hash_partitions_.clear();
}

void PhjBuilder::DoneProbingSinglePartition(
    Partition* partition, std::list<Partition*>* output_partitions, RowBatch* batch) {
  if (NeedToProcessUnmatchedBuildRows(join_op_)) {
    // If the build partition was in memory, we are done probing this partition.
    // In case of right-outer, right-anti and full-outer joins, we move this partition
    // to the list of partitions that we need to output their unmatched build rows.
    output_partitions->push_back(partition);
  } else {
    // In any other case, just close the input build partition.
    partition->Close(IsLeftSemiJoin(join_op_) ? nullptr : batch);
  }
}

void PhjBuilder::CloseAndDeletePartitions(RowBatch* row_batch) {
  // Close all the partitions and clean up all references to them.
  for (unique_ptr<Partition>& partition : all_partitions_) partition->Close(row_batch);
  all_partitions_.clear();
  hash_partitions_.clear();
  null_aware_partition_ = nullptr;
}

void PhjBuilder::AllocateRuntimeFilters() {
  DCHECK(join_op_ != TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN || filter_ctxs_.size() == 0)
      << "Runtime filters not supported with NULL_AWARE_LEFT_ANTI_JOIN";
  DCHECK(ht_ctx_ != nullptr);
  for (int i = 0; i < filter_ctxs_.size(); ++i) {
    if (filter_ctxs_[i].filter->is_bloom_filter()) {
      filter_ctxs_[i].local_bloom_filter =
          runtime_state_->filter_bank()->AllocateScratchBloomFilter(
              filter_ctxs_[i].filter->id());
    } else {
      DCHECK(filter_ctxs_[i].filter->is_min_max_filter());
      filter_ctxs_[i].local_min_max_filter =
          runtime_state_->filter_bank()->AllocateScratchMinMaxFilter(
              filter_ctxs_[i].filter->id(), filter_ctxs_[i].expr_eval->root().type());
    }
  }
}

void PhjBuilder::InsertRuntimeFilters(TupleRow* build_row) noexcept {
  for (const FilterContext& ctx : filter_ctxs_) ctx.Insert(build_row);
}

void PhjBuilder::PublishRuntimeFilters(int64_t num_build_rows) {
  int32_t num_enabled_filters = 0;
  // Use 'num_build_rows' to estimate FP-rate of each Bloom filter, and publish
  // 'always-true' filters if it's too high. Doing so saves CPU at the coordinator,
  // serialisation time, and reduces the cost of applying the filter at the scan - most
  // significantly for per-row filters. However, the number of build rows could be a very
  // poor estimate of the NDV - particularly if the filter expression is a function of
  // several columns.
  // TODO: Better heuristic.
  for (const FilterContext& ctx : filter_ctxs_) {
    // TODO: Consider checking actual number of bits set in filter to compute FP rate.
    // TODO: Consider checking this every few batches or so.
    BloomFilter* bloom_filter = nullptr;
    if (ctx.local_bloom_filter != nullptr) {
      if (runtime_state_->filter_bank()->FpRateTooHigh(
              ctx.filter->filter_size(), num_build_rows)) {
        bloom_filter = BloomFilter::ALWAYS_TRUE_FILTER;
      } else {
        bloom_filter = ctx.local_bloom_filter;
        ++num_enabled_filters;
      }
    } else if (ctx.local_min_max_filter != nullptr
        && !ctx.local_min_max_filter->AlwaysTrue()) {
      ++num_enabled_filters;
    }

    runtime_state_->filter_bank()->UpdateFilterFromLocal(
        ctx.filter->id(), bloom_filter, ctx.local_min_max_filter);
  }

  if (filter_ctxs_.size() > 0) {
    string info_string;
    if (num_enabled_filters == filter_ctxs_.size()) {
      info_string = Substitute("$0 of $0 Runtime Filter$1 Published", filter_ctxs_.size(),
          filter_ctxs_.size() == 1 ? "" : "s");
    } else {
      info_string = Substitute("$0 of $1 Runtime Filter$2 Published, $3 Disabled",
          num_enabled_filters, filter_ctxs_.size(), filter_ctxs_.size() == 1 ? "" : "s",
          filter_ctxs_.size() - num_enabled_filters);
    }
    profile()->AddInfoString("Runtime filters", info_string);
  }
}

Status PhjBuilder::BeginSpilledProbe(bool empty_probe, Partition* partition,
    BufferPool::ClientHandle* probe_client, bool* repartitioned, int* level,
    HashPartitions* new_partitions) {
  DCHECK(partition->is_spilled());
  DCHECK_EQ(0, hash_partitions_.size());

  if (empty_probe) {
    // If there are no probe rows, there's no need to build the hash table, and
    // only partitions with NeedToProcessUnmatcheBuildRows() will have been added
    // to 'spilled_partitions_' in DoneProbingHashPartitions().
    DCHECK(NeedToProcessUnmatchedBuildRows(join_op_));
    bool got_read_buffer = false;
    RETURN_IF_ERROR(partition->build_rows()->PrepareForRead(true, &got_read_buffer));
    if (!got_read_buffer) {
      return mem_tracker()->MemLimitExceeded(
          runtime_state_, Substitute(PREPARE_FOR_READ_FAILED_ERROR_MSG, join_node_id_));
    }
    COUNTER_ADD(num_hash_table_builds_skipped_, 1);
    UpdateState(HashJoinState::PROBING_SPILLED_PARTITION);
    *repartitioned = false;
    *level = partition->level();
    return Status::OK();
  }

  // Set aside memory required for reading the probe stream, so that we don't use
  // it for the hash table.
  buffer_pool_client_->SaveReservation(
      &probe_stream_reservation_, spillable_buffer_size_);

  // Try to build a hash table for the spilled build partition.
  bool built;
  RETURN_IF_ERROR(partition->BuildHashTable(&built));
  if (built) {
    TransferProbeStreamReservation(probe_client);
    UpdateState(HashJoinState::PROBING_SPILLED_PARTITION);
    *repartitioned = false;
    *level = partition->level();
    return Status::OK();
  }
  // This build partition still does not fit in memory, repartition.
  UpdateState(HashJoinState::REPARTITIONING_BUILD);

  int next_partition_level = partition->level() + 1;
  if (UNLIKELY(next_partition_level >= MAX_PARTITION_DEPTH)) {
    return Status(TErrorCode::PARTITIONED_HASH_JOIN_MAX_PARTITION_DEPTH, join_node_id_,
        MAX_PARTITION_DEPTH);
  }

  // Spill to free memory from hash tables and pinned streams for use in new partitions.
  RETURN_IF_ERROR(partition->Spill(BufferedTupleStream::UNPIN_ALL));
  // Temporarily free up the probe reservation to use when repartitioning. Repartitioning
  // will reserve as much memory as needed for the probe streams.
  buffer_pool_client_->RestoreReservation(
      &probe_stream_reservation_, spillable_buffer_size_);

  DCHECK_EQ(partition->build_rows()->BytesPinned(false), 0) << DebugString();
  int64_t num_input_rows = partition->build_rows()->num_rows();
  RETURN_IF_ERROR(RepartitionBuildInput(partition));

  // Check if there was any reduction in the size of partitions after repartitioning.
  int64_t largest_partition_rows = LargestPartitionRows();
  DCHECK_GE(num_input_rows, largest_partition_rows) << "Cannot have a partition with "
                                                       "more rows than the input";
  if (UNLIKELY(num_input_rows == largest_partition_rows)) {
    return Status(TErrorCode::PARTITIONED_HASH_JOIN_REPARTITION_FAILS, join_node_id_,
        next_partition_level, num_input_rows, DebugString(),
        buffer_pool_client_->DebugString());
  }
  TransferProbeStreamReservation(probe_client);
  *repartitioned = true;
  *level = ht_ctx_->level();
  *new_partitions = HashPartitions(ht_ctx_->level(), &hash_partitions_, non_empty_build_);
  return Status::OK();
}

Status PhjBuilder::RepartitionBuildInput(Partition* input_partition) {
  int new_level = input_partition->level() + 1;
  DCHECK_GE(new_level, 1);
  SCOPED_TIMER(repartition_timer_);
  COUNTER_ADD(num_repartitions_, 1);
  RuntimeState* state = runtime_state_;

  // Setup the read buffer and the new partitions.
  BufferedTupleStream* build_rows = input_partition->build_rows();
  DCHECK(build_rows != nullptr);
  bool got_read_buffer;
  RETURN_IF_ERROR(build_rows->PrepareForRead(true, &got_read_buffer));
  if (!got_read_buffer) {
    return mem_tracker()->MemLimitExceeded(
        state, Substitute(PREPARE_FOR_READ_FAILED_ERROR_MSG, join_node_id_));
  }
  RETURN_IF_ERROR(CreateHashPartitions(new_level));

  // Repartition 'input_stream' into 'hash_partitions_'.
  RowBatch build_batch(row_desc_, state->batch_size(), mem_tracker());
  bool eos = false;
  while (!eos) {
    RETURN_IF_CANCELLED(state);
    RETURN_IF_ERROR(state->CheckQueryState());

    RETURN_IF_ERROR(build_rows->GetNext(&build_batch, &eos));
    RETURN_IF_ERROR(Send(state, &build_batch));
    build_batch.Reset();
  }

  // Done reading the input, we can safely close it now to free memory.
  input_partition->Close(nullptr);
  RETURN_IF_ERROR(FlushFinal(state));
  return Status::OK();
}

int64_t PhjBuilder::LargestPartitionRows() const {
  int64_t max_rows = 0;
  for (int i = 0; i < hash_partitions_.size(); ++i) {
    Partition* partition = hash_partitions_[i];
    DCHECK(partition != nullptr);
    if (partition->IsClosed()) continue;
    int64_t rows = partition->build_rows()->num_rows();
    if (rows > max_rows) max_rows = rows;
  }
  return max_rows;
}

bool PhjBuilder::HashTableStoresNulls() const {
  return join_op_ == TJoinOp::RIGHT_OUTER_JOIN || join_op_ == TJoinOp::RIGHT_ANTI_JOIN
      || join_op_ == TJoinOp::FULL_OUTER_JOIN
      || std::accumulate(is_not_distinct_from_.begin(), is_not_distinct_from_.end(),
             false, std::logical_or<bool>());
}

PhjBuilder::Partition::Partition(RuntimeState* state, PhjBuilder* parent, int level)
  : parent_(parent), is_spilled_(false), level_(level) {
  build_rows_ = make_unique<BufferedTupleStream>(state, parent_->row_desc_,
      parent_->buffer_pool_client_, parent->spillable_buffer_size_,
      parent->max_row_buffer_size_);
}

PhjBuilder::Partition::~Partition() {
  DCHECK(IsClosed());
}

int64_t PhjBuilder::Partition::EstimatedInMemSize() const {
  return build_rows_->byte_size() + HashTable::EstimateSize(build_rows_->num_rows());
}

void PhjBuilder::Partition::Close(RowBatch* batch) {
  if (IsClosed()) return;

  if (hash_tbl_ != nullptr) {
    hash_tbl_->StatsCountersAdd(parent_->ht_stats_profile_.get());
    hash_tbl_->Close();
  }

  // Transfer ownership of 'build_rows_' memory to 'batch' if 'batch' is not NULL.
  // Flush out the resources to free up memory for subsequent partitions.
  if (build_rows_ != nullptr) {
    build_rows_->Close(batch, RowBatch::FlushMode::FLUSH_RESOURCES);
    build_rows_.reset();
  }
}

Status PhjBuilder::Partition::Spill(BufferedTupleStream::UnpinMode mode) {
  DCHECK(!IsClosed());
  RETURN_IF_ERROR(parent_->runtime_state_->StartSpilling(parent_->mem_tracker()));
  // Close the hash table and unpin the stream backing it to free memory.
  if (hash_tbl() != nullptr) {
    hash_tbl_->Close();
    hash_tbl_.reset();
  }
  RETURN_IF_ERROR(build_rows_->UnpinStream(mode));
  if (!is_spilled_) {
    COUNTER_ADD(parent_->num_spilled_partitions_, 1);
    if (parent_->num_spilled_partitions_->value() == 1) {
      parent_->profile()->AppendExecOption("Spilled");
    }
  }
  is_spilled_ = true;
  return Status::OK();
}

Status PhjBuilder::Partition::BuildHashTable(bool* built) {
  SCOPED_TIMER(parent_->build_hash_table_timer_);
  DCHECK(build_rows_ != nullptr);
  *built = false;

  // Before building the hash table, we need to pin the rows in memory.
  RETURN_IF_ERROR(build_rows_->PinStream(built));
  if (!*built) return Status::OK();

  RuntimeState* state = parent_->runtime_state_;
  HashTableCtx* ctx = parent_->ht_ctx_.get();
  ctx->set_level(level()); // Set the hash function for building the hash table.
  RowBatch batch(parent_->row_desc_, state->batch_size(), parent_->mem_tracker());
  vector<BufferedTupleStream::FlatRowPtr> flat_rows;
  bool eos = false;

  // Allocate the partition-local hash table. Initialize the number of buckets based on
  // the number of build rows (the number of rows is known at this point). This assumes
  // there are no duplicates which can be wrong. However, the upside in the common case
  // (few/no duplicates) is large and the downside when there are is low (a bit more
  // memory; the bucket memory is small compared to the memory needed for all the build
  // side allocations).
  // One corner case is if the stream contains tuples with zero footprint (no materialized
  // slots). If the tuples occupy no space, this implies all rows will be duplicates, so
  // create a small hash table, IMPALA-2256.
  //
  // TODO: Try to allocate the hash table before pinning the stream to avoid needlessly
  // reading all of the spilled rows from disk when we won't succeed anyway.
  int64_t estimated_num_buckets = HashTable::EstimateNumBuckets(build_rows()->num_rows());
  hash_tbl_.reset(HashTable::Create(parent_->ht_allocator_.get(),
      true /* store_duplicates */, parent_->row_desc_->tuple_descriptors().size(),
      build_rows(), 1 << (32 - NUM_PARTITIONING_BITS), estimated_num_buckets));
  bool success;
  Status status = hash_tbl_->Init(&success);
  if (!status.ok() || !success) goto not_built;
  status = build_rows_->PrepareForRead(false, &success);
  if (!status.ok()) goto not_built;
  DCHECK(success) << "Stream was already pinned.";

  do {
    status = build_rows_->GetNext(&batch, &eos, &flat_rows);
    if (!status.ok()) goto not_built;
    DCHECK_EQ(batch.num_rows(), flat_rows.size());
    DCHECK_LE(batch.num_rows(), hash_tbl_->EmptyBuckets());
    TPrefetchMode::type prefetch_mode = state->query_options().prefetch_mode;
    if (parent_->insert_batch_fn_ != nullptr) {
      InsertBatchFn insert_batch_fn;
      if (level() == 0) {
        insert_batch_fn = parent_->insert_batch_fn_level0_;
      } else {
        insert_batch_fn = parent_->insert_batch_fn_;
      }
      DCHECK(insert_batch_fn != nullptr);
      if (UNLIKELY(
              !insert_batch_fn(this, prefetch_mode, ctx, &batch, flat_rows, &status))) {
        goto not_built;
      }
    } else if (UNLIKELY(!InsertBatch(prefetch_mode, ctx, &batch, flat_rows, &status))) {
      goto not_built;
    }
    RETURN_IF_CANCELLED(state);
    RETURN_IF_ERROR(state->GetQueryStatus());
    // Free any expr result allocations made while inserting.
    parent_->expr_results_pool_->Clear();
    batch.Reset();
  } while (!eos);

  // The hash table fits in memory and is built.
  DCHECK(*built);
  DCHECK(hash_tbl_ != nullptr);
  is_spilled_ = false;
  COUNTER_ADD(parent_->ht_stats_profile_->num_hash_buckets_,
      hash_tbl_->num_buckets());
  return Status::OK();

not_built:
  *built = false;
  if (hash_tbl_ != nullptr) {
    hash_tbl_->Close();
    hash_tbl_.reset();
  }
  return status;
}

std::string PhjBuilder::Partition::DebugString() {
  stringstream ss;
  ss << "<Partition>: ptr=" << this;
  if (IsClosed()) {
    ss << " Closed";
    return ss.str();
  }
  if (is_spilled()) {
    ss << " Spilled";
  }
  DCHECK(build_rows() != nullptr);
  ss << endl
     << "    Build Rows: " << build_rows_->num_rows()
     << " (Bytes pinned: " << build_rows_->BytesPinned(false) << ")"
     << endl;
  if (hash_tbl_ != nullptr) {
    ss << "    Hash Table Rows: " << hash_tbl_->size();
  }
  return ss.str();
}

void PhjBuilder::Codegen(LlvmCodeGen* codegen) {
  Status build_codegen_status;
  Status insert_codegen_status;
  Status codegen_status;

  // Codegen for hashing rows with the builder's hash table context.
  llvm::Function* hash_fn;
  codegen_status = ht_ctx_->CodegenHashRow(codegen, false, &hash_fn);
  llvm::Function* murmur_hash_fn;
  codegen_status.MergeStatus(ht_ctx_->CodegenHashRow(codegen, true, &murmur_hash_fn));

  // Codegen for evaluating build rows
  llvm::Function* eval_build_row_fn;
  codegen_status.MergeStatus(ht_ctx_->CodegenEvalRow(codegen, true, &eval_build_row_fn));

  llvm::Function* insert_filters_fn;
  codegen_status.MergeStatus(
      CodegenInsertRuntimeFilters(codegen, filter_exprs_, &insert_filters_fn));

  if (codegen_status.ok()) {
    TPrefetchMode::type prefetch_mode = runtime_state_->query_options().prefetch_mode;
    build_codegen_status = CodegenProcessBuildBatch(
        codegen, hash_fn, murmur_hash_fn, eval_build_row_fn, insert_filters_fn);
    insert_codegen_status = CodegenInsertBatch(codegen, hash_fn, murmur_hash_fn,
        eval_build_row_fn, prefetch_mode);
  } else {
    build_codegen_status = codegen_status;
    insert_codegen_status = codegen_status;
  }
  profile()->AddCodegenMsg(build_codegen_status.ok(), build_codegen_status, "Build Side");
  profile()->AddCodegenMsg(insert_codegen_status.ok(), insert_codegen_status,
      "Hash Table Construction");
}

string PhjBuilder::DebugString() const {
  stringstream ss;
  ss << " PhjBuilder state=" << PrintState()
     << " Hash partitions: " << hash_partitions_.size() << ":" << endl;
  for (int i = 0; i < hash_partitions_.size(); ++i) {
    ss << " Hash partition " << i << " " << hash_partitions_[i]->DebugString() << endl;
  }
  if (null_aware_partition_ != nullptr) {
    ss << "Null-aware partition: " << null_aware_partition_->DebugString();
  }
  return ss.str();
}

Status PhjBuilder::CodegenProcessBuildBatch(LlvmCodeGen* codegen, llvm::Function* hash_fn,
    llvm::Function* murmur_hash_fn, llvm::Function* eval_row_fn,
    llvm::Function* insert_filters_fn) {
  llvm::Function* process_build_batch_fn =
      codegen->GetFunction(IRFunction::PHJ_PROCESS_BUILD_BATCH, true);
  DCHECK(process_build_batch_fn != nullptr);

  // Replace call sites
  int replaced =
      codegen->ReplaceCallSites(process_build_batch_fn, eval_row_fn, "EvalBuildRow");
  DCHECK_REPLACE_COUNT(replaced, 1);

  replaced = codegen->ReplaceCallSites(
      process_build_batch_fn, insert_filters_fn, "InsertRuntimeFilters");
  DCHECK_REPLACE_COUNT(replaced, 1);

  // Replace some hash table parameters with constants.
  HashTableCtx::HashTableReplacedConstants replaced_constants;
  const bool stores_duplicates = true;
  const int num_build_tuples = row_desc_->tuple_descriptors().size();
  RETURN_IF_ERROR(ht_ctx_->ReplaceHashTableConstants(codegen, stores_duplicates,
      num_build_tuples, process_build_batch_fn, &replaced_constants));
  DCHECK_GE(replaced_constants.stores_nulls, 1);
  DCHECK_EQ(replaced_constants.finds_some_nulls, 0);
  DCHECK_EQ(replaced_constants.stores_duplicates, 0);
  DCHECK_EQ(replaced_constants.stores_tuples, 0);
  DCHECK_EQ(replaced_constants.quadratic_probing, 0);

  llvm::Value* is_null_aware_arg = codegen->GetArgument(process_build_batch_fn, 5);
  is_null_aware_arg->replaceAllUsesWith(
      codegen->GetBoolConstant(join_op_ == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN));

  llvm::Function* process_build_batch_fn_level0 =
      codegen->CloneFunction(process_build_batch_fn);

  // Always build runtime filters at level0 (if there are any).
  // Note that the first argument of this function is the return value.
  llvm::Value* build_filter_l0_arg =
      codegen->GetArgument(process_build_batch_fn_level0, 4);
  build_filter_l0_arg->replaceAllUsesWith(
      codegen->GetBoolConstant(filter_ctxs_.size() > 0));

  // process_build_batch_fn_level0 uses CRC hash if available,
  replaced =
      codegen->ReplaceCallSites(process_build_batch_fn_level0, hash_fn, "HashRow");
  DCHECK_REPLACE_COUNT(replaced, 1);

  // process_build_batch_fn uses murmur
  replaced =
      codegen->ReplaceCallSites(process_build_batch_fn, murmur_hash_fn, "HashRow");
  DCHECK_REPLACE_COUNT(replaced, 1);

  // Never build filters after repartitioning, as all rows have already been added to the
  // filters during the level0 build. Note that the first argument of this function is the
  // return value.
  llvm::Value* build_filter_arg = codegen->GetArgument(process_build_batch_fn, 4);
  build_filter_arg->replaceAllUsesWith(codegen->false_value());

  // Finalize ProcessBuildBatch functions
  process_build_batch_fn = codegen->FinalizeFunction(process_build_batch_fn);
  if (process_build_batch_fn == nullptr) {
    return Status(
        "Codegen'd PhjBuilder::ProcessBuildBatch() function "
        "failed verification, see log");
  }
  process_build_batch_fn_level0 =
      codegen->FinalizeFunction(process_build_batch_fn_level0);
  if (process_build_batch_fn == nullptr) {
    return Status(
        "Codegen'd level-zero PhjBuilder::ProcessBuildBatch() "
        "function failed verification, see log");
  }

  // Register native function pointers
  codegen->AddFunctionToJit(
      process_build_batch_fn, reinterpret_cast<void**>(&process_build_batch_fn_));
  codegen->AddFunctionToJit(process_build_batch_fn_level0,
      reinterpret_cast<void**>(&process_build_batch_fn_level0_));
  return Status::OK();
}

Status PhjBuilder::CodegenInsertBatch(LlvmCodeGen* codegen, llvm::Function* hash_fn,
    llvm::Function* murmur_hash_fn, llvm::Function* eval_row_fn,
    TPrefetchMode::type prefetch_mode) {
  llvm::Function* insert_batch_fn =
      codegen->GetFunction(IRFunction::PHJ_INSERT_BATCH, true);
  llvm::Function* build_equals_fn;
  RETURN_IF_ERROR(ht_ctx_->CodegenEquals(codegen, true, &build_equals_fn));

  // Replace the parameter 'prefetch_mode' with constant.
  llvm::Value* prefetch_mode_arg = codegen->GetArgument(insert_batch_fn, 1);
  DCHECK_GE(prefetch_mode, TPrefetchMode::NONE);
  DCHECK_LE(prefetch_mode, TPrefetchMode::HT_BUCKET);
  prefetch_mode_arg->replaceAllUsesWith(codegen->GetI32Constant(prefetch_mode));

  // Use codegen'd EvalBuildRow() function
  int replaced = codegen->ReplaceCallSites(insert_batch_fn, eval_row_fn, "EvalBuildRow");
  DCHECK_REPLACE_COUNT(replaced, 1);

  // Use codegen'd Equals() function
  replaced = codegen->ReplaceCallSites(insert_batch_fn, build_equals_fn, "Equals");
  DCHECK_REPLACE_COUNT(replaced, 1);

  // Replace hash-table parameters with constants.
  HashTableCtx::HashTableReplacedConstants replaced_constants;
  const bool stores_duplicates = true;
  const int num_build_tuples = row_desc_->tuple_descriptors().size();
  RETURN_IF_ERROR(ht_ctx_->ReplaceHashTableConstants(codegen, stores_duplicates,
      num_build_tuples, insert_batch_fn, &replaced_constants));
  DCHECK_GE(replaced_constants.stores_nulls, 1);
  DCHECK_EQ(replaced_constants.finds_some_nulls, 0);
  DCHECK_GE(replaced_constants.stores_duplicates, 1);
  DCHECK_GE(replaced_constants.stores_tuples, 1);
  DCHECK_GE(replaced_constants.quadratic_probing, 1);

  llvm::Function* insert_batch_fn_level0 = codegen->CloneFunction(insert_batch_fn);

  // Use codegen'd hash functions
  replaced = codegen->ReplaceCallSites(insert_batch_fn_level0, hash_fn, "HashRow");
  DCHECK_REPLACE_COUNT(replaced, 1);
  replaced = codegen->ReplaceCallSites(insert_batch_fn, murmur_hash_fn, "HashRow");
  DCHECK_REPLACE_COUNT(replaced, 1);

  insert_batch_fn = codegen->FinalizeFunction(insert_batch_fn);
  if (insert_batch_fn == nullptr) {
    return Status(
        "PartitionedHashJoinNode::CodegenInsertBatch(): codegen'd "
        "InsertBatch() function failed verification, see log");
  }
  insert_batch_fn_level0 = codegen->FinalizeFunction(insert_batch_fn_level0);
  if (insert_batch_fn_level0 == nullptr) {
    return Status(
        "PartitionedHashJoinNode::CodegenInsertBatch(): codegen'd zero-level "
        "InsertBatch() function failed verification, see log");
  }

  codegen->AddFunctionToJit(insert_batch_fn, reinterpret_cast<void**>(&insert_batch_fn_));
  codegen->AddFunctionToJit(
      insert_batch_fn_level0, reinterpret_cast<void**>(&insert_batch_fn_level0_));
  return Status::OK();
}

// An example of the generated code for a query with two filters built by this node.
//
// ; Function Attrs: noinline
// define void @InsertRuntimeFilters(%"class.impala::PhjBuilder"* %this,
//     %"class.impala::TupleRow"* %row) #46 {
// entry:
//   call void @FilterContextInsert(%"struct.impala::FilterContext"* inttoptr (
//       i64 197870464 to %"struct.impala::FilterContext"*),
//       %"class.impala::TupleRow"* %row)
//   call void @FilterContextInsert.14(%"struct.impala::FilterContext"* inttoptr (
//       i64 197870496 to %"struct.impala::FilterContext"*),
//       %"class.impala::TupleRow"* %row)
//   ret void
// }
Status PhjBuilder::CodegenInsertRuntimeFilters(
    LlvmCodeGen* codegen, const vector<ScalarExpr*>& filter_exprs, llvm::Function** fn) {
  llvm::LLVMContext& context = codegen->context();
  LlvmBuilder builder(context);

  *fn = nullptr;
  llvm::Type* this_type = codegen->GetStructPtrType<PhjBuilder>();
  llvm::PointerType* tuple_row_ptr_type = codegen->GetStructPtrType<TupleRow>();
  LlvmCodeGen::FnPrototype prototype(
      codegen, "InsertRuntimeFilters", codegen->void_type());
  prototype.AddArgument(LlvmCodeGen::NamedVariable("this", this_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("row", tuple_row_ptr_type));

  llvm::Value* args[2];
  llvm::Function* insert_runtime_filters_fn = prototype.GeneratePrototype(&builder, args);
  llvm::Value* row_arg = args[1];

  int num_filters = filter_exprs.size();
  for (int i = 0; i < num_filters; ++i) {
    llvm::Function* insert_fn;
    RETURN_IF_ERROR(FilterContext::CodegenInsert(
        codegen, filter_exprs_[i], &filter_ctxs_[i], &insert_fn));
    llvm::PointerType* filter_context_type = codegen->GetStructPtrType<FilterContext>();
    llvm::Value* filter_context_ptr =
        codegen->CastPtrToLlvmPtr(filter_context_type, &filter_ctxs_[i]);

    llvm::Value* insert_args[] = {filter_context_ptr, row_arg};
    builder.CreateCall(insert_fn, insert_args);
  }

  builder.CreateRetVoid();

  if (num_filters > 0) {
    // Don't inline this function to avoid code bloat in ProcessBuildBatch().
    // If there is any filter, InsertRuntimeFilters() is large enough to not benefit
    // much from inlining.
    insert_runtime_filters_fn->addFnAttr(llvm::Attribute::NoInline);
  }

  *fn = codegen->FinalizeFunction(insert_runtime_filters_fn);
  if (*fn == nullptr) {
    return Status("Codegen'd PhjBuilder::InsertRuntimeFilters() failed "
                  "verification, see log");
  }
  return Status::OK();
}
