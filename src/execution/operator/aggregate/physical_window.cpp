#include "duckdb/execution/operator/aggregate/physical_window.hpp"

#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/sort/partition_state.hpp"

#include "duckdb/common/types/column/column_data_consumer.hpp"
#include "duckdb/common/types/row/row_data_collection_scanner.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/windows_undefs.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/window_executor.hpp"
#include "duckdb/execution/window_segment_tree.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace duckdb {

//	Global sink state
class WindowGlobalSinkState;

enum WindowGroupStage : uint8_t { SINK, FINALIZE, GETDATA, DONE };

class WindowHashGroup {
public:
	using HashGroupPtr = unique_ptr<PartitionGlobalHashGroup>;
	using OrderMasks = PartitionGlobalHashGroup::OrderMasks;
	using ExecutorGlobalStatePtr = unique_ptr<WindowExecutorGlobalState>;
	using ExecutorGlobalStates = vector<ExecutorGlobalStatePtr>;

	WindowHashGroup(WindowGlobalSinkState &gstate, const idx_t hash_bin_p);

	ExecutorGlobalStates &Initialize(WindowGlobalSinkState &gstate);

	// Scan all of the blocks during the build phase
	unique_ptr<RowDataCollectionScanner> GetBuildScanner(idx_t block_idx) const {
		if (!rows) {
			return nullptr;
		}
		return make_uniq<RowDataCollectionScanner>(*rows, *heap, layout, external, block_idx, false);
	}

	// Scan a single block during the evaluate phase
	unique_ptr<RowDataCollectionScanner> GetEvaluateScanner(idx_t block_idx) const {
		//	Second pass can flush
		D_ASSERT(rows);
		return make_uniq<RowDataCollectionScanner>(*rows, *heap, layout, external, block_idx, true);
	}

	// The processing stage for this group
	WindowGroupStage GetStage() const {
		auto result = WindowGroupStage::SINK;

		if (sunk == count) {
			result = WindowGroupStage::FINALIZE;
		}

		if (finalized == blocks) {
			result = WindowGroupStage::GETDATA;
		}

		return result;
	}

	//! The hash partition data
	HashGroupPtr hash_group;
	//! The size of the group
	idx_t count = 0;
	//! The number of blocks in the group
	idx_t blocks = 0;
	unique_ptr<RowDataCollection> rows;
	unique_ptr<RowDataCollection> heap;
	RowLayout layout;
	//! The partition boundary mask
	ValidityMask partition_mask;
	//! The order boundary mask
	OrderMasks order_masks;
	//! External paging
	bool external;
	//! The function global states for this hash group
	ExecutorGlobalStates gestates;

	//! The bin number
	idx_t hash_bin;
	//! Single threading lock
	mutex lock;
	//! Count of sunk rows
	std::atomic<idx_t> sunk;
	//! Count of finalized blocks
	std::atomic<idx_t> finalized;
	//! The number of tasks left before we should be deleted
	std::atomic<idx_t> tasks_remaining;
	//! The output ordering batch index this hash group starts at
	idx_t batch_base;

private:
	void MaterializeSortedData();
};

class WindowPartitionGlobalSinkState;

class WindowGlobalSinkState : public GlobalSinkState {
public:
	using ExecutorPtr = unique_ptr<WindowExecutor>;
	using Executors = vector<ExecutorPtr>;

	WindowGlobalSinkState(const PhysicalWindow &op, ClientContext &context);

	//! Parent operator
	const PhysicalWindow &op;
	//! Execution context
	ClientContext &context;
	//! The partitioned sunk data
	unique_ptr<WindowPartitionGlobalSinkState> global_partition;
	//! The execution functions
	Executors executors;
};

class WindowPartitionGlobalSinkState : public PartitionGlobalSinkState {
public:
	using WindowHashGroupPtr = unique_ptr<WindowHashGroup>;

	WindowPartitionGlobalSinkState(WindowGlobalSinkState &gsink, const BoundWindowExpression &wexpr)
	    : PartitionGlobalSinkState(gsink.context, wexpr.partitions, wexpr.orders, gsink.op.children[0]->types,
	                               wexpr.partitions_stats, gsink.op.estimated_cardinality),
	      gsink(gsink) {
	}
	~WindowPartitionGlobalSinkState() override = default;

	void OnBeginMerge() override {
		PartitionGlobalSinkState::OnBeginMerge();
		window_hash_groups.resize(hash_groups.size());
	}

	void OnSortedPartition(const idx_t group_idx) override {
		PartitionGlobalSinkState::OnSortedPartition(group_idx);
		window_hash_groups[group_idx] = make_uniq<WindowHashGroup>(gsink, group_idx);
	}

	//! Operator global sink state
	WindowGlobalSinkState &gsink;
	//! The sorted hash groups
	vector<WindowHashGroupPtr> window_hash_groups;
};

//	Per-thread sink state
class WindowLocalSinkState : public LocalSinkState {
public:
	WindowLocalSinkState(ClientContext &context, const WindowGlobalSinkState &gstate)
	    : local_partition(context, *gstate.global_partition) {
	}

	void Sink(DataChunk &input_chunk) {
		local_partition.Sink(input_chunk);
	}

	void Combine() {
		local_partition.Combine();
	}

	PartitionLocalSinkState local_partition;
};

// this implements a sorted window functions variant
PhysicalWindow::PhysicalWindow(vector<LogicalType> types, vector<unique_ptr<Expression>> select_list_p,
                               idx_t estimated_cardinality, PhysicalOperatorType type)
    : PhysicalOperator(type, std::move(types), estimated_cardinality), select_list(std::move(select_list_p)),
      order_idx(0), is_order_dependent(false) {

	idx_t max_orders = 0;
	for (idx_t i = 0; i < select_list.size(); ++i) {
		auto &expr = select_list[i];
		D_ASSERT(expr->expression_class == ExpressionClass::BOUND_WINDOW);
		auto &bound_window = expr->Cast<BoundWindowExpression>();
		if (bound_window.partitions.empty() && bound_window.orders.empty()) {
			is_order_dependent = true;
		}

		if (bound_window.orders.size() > max_orders) {
			order_idx = i;
			max_orders = bound_window.orders.size();
		}
	}
}

static unique_ptr<WindowExecutor> WindowExecutorFactory(BoundWindowExpression &wexpr, ClientContext &context,
                                                        WindowAggregationMode mode) {
	switch (wexpr.type) {
	case ExpressionType::WINDOW_AGGREGATE:
		return make_uniq<WindowAggregateExecutor>(wexpr, context, mode);
	case ExpressionType::WINDOW_ROW_NUMBER:
		return make_uniq<WindowRowNumberExecutor>(wexpr, context);
	case ExpressionType::WINDOW_RANK_DENSE:
		return make_uniq<WindowDenseRankExecutor>(wexpr, context);
	case ExpressionType::WINDOW_RANK:
		return make_uniq<WindowRankExecutor>(wexpr, context);
	case ExpressionType::WINDOW_PERCENT_RANK:
		return make_uniq<WindowPercentRankExecutor>(wexpr, context);
	case ExpressionType::WINDOW_CUME_DIST:
		return make_uniq<WindowCumeDistExecutor>(wexpr, context);
	case ExpressionType::WINDOW_NTILE:
		return make_uniq<WindowNtileExecutor>(wexpr, context);
	case ExpressionType::WINDOW_LEAD:
	case ExpressionType::WINDOW_LAG:
		return make_uniq<WindowLeadLagExecutor>(wexpr, context);
	case ExpressionType::WINDOW_FIRST_VALUE:
		return make_uniq<WindowFirstValueExecutor>(wexpr, context);
	case ExpressionType::WINDOW_LAST_VALUE:
		return make_uniq<WindowLastValueExecutor>(wexpr, context);
	case ExpressionType::WINDOW_NTH_VALUE:
		return make_uniq<WindowNthValueExecutor>(wexpr, context);
		break;
	default:
		throw InternalException("Window aggregate type %s", ExpressionTypeToString(wexpr.type));
	}
}

WindowGlobalSinkState::WindowGlobalSinkState(const PhysicalWindow &op, ClientContext &context)
    : op(op), context(context) {

	D_ASSERT(op.select_list[op.order_idx]->GetExpressionClass() == ExpressionClass::BOUND_WINDOW);
	auto &wexpr = op.select_list[op.order_idx]->Cast<BoundWindowExpression>();

	const auto mode = DBConfig::GetConfig(context).options.window_mode;
	for (idx_t expr_idx = 0; expr_idx < op.select_list.size(); ++expr_idx) {
		D_ASSERT(op.select_list[expr_idx]->GetExpressionClass() == ExpressionClass::BOUND_WINDOW);
		auto &wexpr = op.select_list[expr_idx]->Cast<BoundWindowExpression>();
		auto wexec = WindowExecutorFactory(wexpr, context, mode);
		executors.emplace_back(std::move(wexec));
	}

	global_partition = make_uniq<WindowPartitionGlobalSinkState>(*this, wexpr);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PhysicalWindow::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<WindowLocalSinkState>();

	lstate.Sink(chunk);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalWindow::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &lstate = input.local_state.Cast<WindowLocalSinkState>();
	lstate.Combine();

	return SinkCombineResultType::FINISHED;
}

unique_ptr<LocalSinkState> PhysicalWindow::GetLocalSinkState(ExecutionContext &context) const {
	auto &gstate = sink_state->Cast<WindowGlobalSinkState>();
	return make_uniq<WindowLocalSinkState>(context.client, gstate);
}

unique_ptr<GlobalSinkState> PhysicalWindow::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<WindowGlobalSinkState>(*this, context);
}

SinkFinalizeType PhysicalWindow::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
	auto &state = input.global_state.Cast<WindowGlobalSinkState>();

	//	Did we get any data?
	if (!state.global_partition->count) {
		return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}

	// Do we have any sorting to schedule?
	if (state.global_partition->rows) {
		D_ASSERT(!state.global_partition->grouping_data);
		return state.global_partition->rows->count ? SinkFinalizeType::READY : SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}

	// Find the first group to sort
	if (!state.global_partition->HasMergeTasks()) {
		// Empty input!
		return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}

	// Schedule all the sorts for maximum thread utilisation
	auto new_event = make_shared_ptr<PartitionMergeEvent>(*state.global_partition, pipeline);
	event.InsertEvent(std::move(new_event));

	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class WindowGlobalSourceState : public GlobalSourceState {
public:
	using ScannerPtr = unique_ptr<RowDataCollectionScanner>;

	struct Task {
		Task(WindowGroupStage stage, idx_t group_idx, idx_t max_idx)
		    : stage(stage), group_idx(group_idx), max_idx(max_idx) {
		}
		WindowGroupStage stage;
		idx_t group_idx;
		idx_t max_idx;
		idx_t begin_idx = 0;
		idx_t end_idx = 0;
	};
	using TaskPtr = optional_ptr<Task>;

	WindowGlobalSourceState(ClientContext &context_p, WindowGlobalSinkState &gsink_p);

	//! Get the next task given the current state
	bool TryNextTask(TaskPtr &task);
	//! Finish a task
	void FinishTask(TaskPtr task);
	//! Single-threaded manipulation of the interrupt queue
	bool UpdateBlockedTasks(bool blocked, InterruptState &interrupt_state) const;

	//! Context for executing computations
	ClientContext &context;
	//! All the sunk data
	WindowGlobalSinkState &gsink;
	//! State mutex
	mutable mutex lock;
	//! The list of tasks
	vector<Task> tasks;
	//! The the next task
	idx_t next_task;
	//! Stop producing tasks
	atomic<bool> stopped;
	//! The number of rows returned
	atomic<idx_t> returned;
	//! The set of blocked tasks
	mutable vector<InterruptState> blocked_tasks;

public:
	idx_t MaxThreads() override {
		return tasks.size();
	}
};

WindowGlobalSourceState::WindowGlobalSourceState(ClientContext &context_p, WindowGlobalSinkState &gsink_p)
    : context(context_p), gsink(gsink_p), next_task(0), stopped(false), returned(0) {
	auto &gpart = gsink.global_partition;
	auto &window_hash_groups = gsink.global_partition->window_hash_groups;

	if (window_hash_groups.empty()) {
		//	OVER()
		if (gpart->rows && !gpart->rows->blocks.empty()) {
			// We need to construct the single WindowHashGroup here because the sort tasks will not be run.
			window_hash_groups.emplace_back(make_uniq<WindowHashGroup>(gsink, idx_t(0)));
		}
	} else {
		idx_t batch_base = 0;
		for (auto &window_hash_group : window_hash_groups) {
			if (!window_hash_group) {
				continue;
			}
			auto &rows = window_hash_group->rows;
			if (!rows) {
				continue;
			}

			const auto block_count = window_hash_group->rows->blocks.size();
			window_hash_group->batch_base = batch_base;
			batch_base += block_count;
		}
	}

	//    Sort the groups from largest to smallest
	if (window_hash_groups.empty()) {
		return;
	}

	using PartitionBlock = std::pair<idx_t, idx_t>;
	vector<PartitionBlock> partition_blocks;
	for (idx_t group_idx = 0; group_idx < window_hash_groups.size(); ++group_idx) {
		auto &window_hash_group = window_hash_groups[group_idx];
		partition_blocks.emplace_back(window_hash_group->rows->blocks.size(), group_idx);
	}
	std::sort(partition_blocks.begin(), partition_blocks.end(), std::greater<PartitionBlock>());

	//	Schedule the largest group on as many threads as possible
	const auto threads = idx_t(TaskScheduler::GetScheduler(context).NumberOfThreads());
	const auto &max_block = partition_blocks.front();
	const auto per_thread = (max_block.first + threads - 1) / threads;

	//	TODO: Generate dynamically instead of building a big list?
	vector<WindowGroupStage> states {WindowGroupStage::SINK, WindowGroupStage::FINALIZE, WindowGroupStage::GETDATA};
	for (const auto &b : partition_blocks) {
		for (const auto &state : states) {
			for (Task task(state, b.second, b.first); task.begin_idx < task.max_idx; task.begin_idx += per_thread) {
				task.end_idx = MinValue<idx_t>(task.begin_idx + per_thread, task.max_idx);
				tasks.emplace_back(task);
				window_hash_groups[task.group_idx]->tasks_remaining++;
			}
		}
	}
}

void WindowHashGroup::MaterializeSortedData() {
	auto &global_sort_state = *hash_group->global_sort;
	if (global_sort_state.sorted_blocks.empty()) {
		return;
	}

	// scan the sorted row data
	D_ASSERT(global_sort_state.sorted_blocks.size() == 1);
	auto &sb = *global_sort_state.sorted_blocks[0];

	// Free up some memory before allocating more
	sb.radix_sorting_data.clear();
	sb.blob_sorting_data = nullptr;

	// Move the sorting row blocks into our RDCs
	auto &buffer_manager = global_sort_state.buffer_manager;
	auto &sd = *sb.payload_data;

	// Data blocks are required
	D_ASSERT(!sd.data_blocks.empty());
	auto &block = sd.data_blocks[0];
	rows = make_uniq<RowDataCollection>(buffer_manager, block->capacity, block->entry_size);
	rows->blocks = std::move(sd.data_blocks);
	rows->count = std::accumulate(rows->blocks.begin(), rows->blocks.end(), idx_t(0),
	                              [&](idx_t c, const unique_ptr<RowDataBlock> &b) { return c + b->count; });

	// Heap blocks are optional, but we want both for iteration.
	if (!sd.heap_blocks.empty()) {
		auto &block = sd.heap_blocks[0];
		heap = make_uniq<RowDataCollection>(buffer_manager, block->capacity, block->entry_size);
		heap->blocks = std::move(sd.heap_blocks);
		hash_group.reset();
	} else {
		heap = make_uniq<RowDataCollection>(buffer_manager, buffer_manager.GetBlockSize(), 1U, true);
	}
	heap->count = std::accumulate(heap->blocks.begin(), heap->blocks.end(), idx_t(0),
	                              [&](idx_t c, const unique_ptr<RowDataBlock> &b) { return c + b->count; });
}

WindowHashGroup::WindowHashGroup(WindowGlobalSinkState &gstate, const idx_t hash_bin_p)
    : count(0), blocks(0), hash_bin(hash_bin_p), sunk(0), finalized(0), tasks_remaining(0), batch_base(0) {
	// There are three types of partitions:
	// 1. No partition (no sorting)
	// 2. One partition (sorting, but no hashing)
	// 3. Multiple partitions (sorting and hashing)

	//	How big is the partition?
	auto &gpart = *gstate.global_partition;
	layout.Initialize(gpart.payload_types);
	if (hash_bin < gpart.hash_groups.size() && gpart.hash_groups[hash_bin]) {
		count = gpart.hash_groups[hash_bin]->count;
	} else if (gpart.rows && !hash_bin) {
		count = gpart.count;
	} else {
		return;
	}

	//	Initialise masks to false
	partition_mask.Initialize(count);
	partition_mask.SetAllInvalid(count);

	const auto &executors = gstate.executors;
	for (auto &wexec : executors) {
		auto &wexpr = wexec->wexpr;
		auto &order_mask = order_masks[wexpr.partitions.size() + wexpr.orders.size()];
		if (order_mask.IsMaskSet()) {
			continue;
		}
		order_mask.Initialize(count);
		order_mask.SetAllInvalid(count);
	}

	// Scan the sorted data into new Collections
	external = gpart.external;
	if (gpart.rows && !hash_bin) {
		// Simple mask
		partition_mask.SetValidUnsafe(0);
		for (auto &order_mask : order_masks) {
			order_mask.second.SetValidUnsafe(0);
		}
		//	No partition - align the heap blocks with the row blocks
		rows = gpart.rows->CloneEmpty(gpart.rows->keep_pinned);
		heap = gpart.strings->CloneEmpty(gpart.strings->keep_pinned);
		RowDataCollectionScanner::AlignHeapBlocks(*rows, *heap, *gpart.rows, *gpart.strings, layout);
		external = true;
	} else if (hash_bin < gpart.hash_groups.size()) {
		// Overwrite the collections with the sorted data
		D_ASSERT(gpart.hash_groups[hash_bin].get());
		hash_group = std::move(gpart.hash_groups[hash_bin]);
		hash_group->ComputeMasks(partition_mask, order_masks);
		external = hash_group->global_sort->external;
		MaterializeSortedData();
	}

	if (rows) {
		blocks = rows->blocks.size();
	}
}

// Per-thread scan state
class WindowLocalSourceState : public LocalSourceState {
public:
	using LocalStatePtr = unique_ptr<WindowExecutorLocalState>;
	using LocalStates = vector<LocalStatePtr>;
	using Task = WindowGlobalSourceState::Task;
	using TaskPtr = optional_ptr<Task>;

	explicit WindowLocalSourceState(WindowGlobalSourceState &gsource);
	void BeginHashGroup();
	void Sink();
	void Finalize();
	bool GetData(DataChunk &chunk);
	void FinishHashGroup(TaskPtr prev_task);

	//! The shared source state
	WindowGlobalSourceState &gsource;
	//! The current batch index (for output reordering)
	idx_t batch_index;
	//! The task this thread is working on
	TaskPtr task;
	//! The current source being processed
	optional_ptr<WindowHashGroup> window_hash_group;
	//! The scan cursor
	unique_ptr<RowDataCollectionScanner> scanner;
	//! Buffer for the inputs
	DataChunk input_chunk;
	//! Executor local states.
	LocalStates local_states;
	//! Buffer for window results
	DataChunk output_chunk;
};

WindowHashGroup::ExecutorGlobalStates &WindowHashGroup::Initialize(WindowGlobalSinkState &gsink) {
	//	Single-threaded building as this is mostly memory allocation
	lock_guard<mutex> gestate_guard(lock);
	const auto &executors = gsink.executors;
	if (gestates.size() == executors.size()) {
		return gestates;
	}

	// These can be large so we defer building them until we are ready.
	for (auto &wexec : executors) {
		auto &wexpr = wexec->wexpr;
		auto &order_mask = order_masks[wexpr.partitions.size() + wexpr.orders.size()];
		gestates.emplace_back(wexec->GetGlobalState(count, partition_mask, order_mask));
	}

	return gestates;
}

void WindowLocalSourceState::BeginHashGroup() {
	if (!task) {
		return;
	}

	auto &gsink = gsource.gsink;
	auto &gpart = *gsink.global_partition;
	window_hash_group = gpart.window_hash_groups[task->group_idx].get();

	// Create the executor state for each function
	// These can be large so we defer building them until we are ready.
	auto &gestates = window_hash_group->Initialize(gsink);

	//	Set up the local states
	const auto &executors = gsink.executors;
	for (idx_t w = 0; w < executors.size(); ++w) {
		local_states.emplace_back(executors[w]->GetLocalState(*gestates[w]));
	}
}

void WindowLocalSourceState::Sink() {
	D_ASSERT(task->stage == WindowGroupStage::SINK);

	auto &gsink = gsource.gsink;
	const auto &executors = gsink.executors;
	auto &gestates = window_hash_group->gestates;

	//	First pass over the input without flushing
	for (; task->begin_idx < task->end_idx; ++task->begin_idx) {
		scanner = window_hash_group->GetBuildScanner(task->begin_idx);
		if (!scanner) {
			break;
		}
		while (true) {
			//	TODO: Try to align on validity mask boundaries by starting ragged?
			idx_t input_idx = scanner->Scanned();
			input_chunk.Reset();
			scanner->Scan(input_chunk);
			if (input_chunk.size() == 0) {
				break;
			}

			//	TODO: Stagger functions to reduce contention?
			for (idx_t w = 0; w < executors.size(); ++w) {
				executors[w]->Sink(input_chunk, input_idx, scanner->Count(), *gestates[w], *local_states[w]);
			}

			window_hash_group->sunk += input_chunk.size();
		}

		// External scanning assumes all blocks are swizzled.
		scanner->SwizzleBlock(task->begin_idx);
		scanner.reset();
	}
}

void WindowLocalSourceState::Finalize() {
	D_ASSERT(task->stage == WindowGroupStage::FINALIZE);

	// Finalize all the executors.
	// Parallel finalisation is handled internally by the executor,
	// and should not return until all threads have completed work.
	auto &gsink = gsource.gsink;
	const auto &executors = gsink.executors;
	auto &gestates = window_hash_group->gestates;
	for (idx_t w = 0; w < executors.size(); ++w) {
		executors[w]->Finalize(*gestates[w], *local_states[w]);
	}

	//	Mark this range as done
	window_hash_group->finalized += (task->end_idx - task->begin_idx);
	task->begin_idx = task->end_idx;
}

WindowLocalSourceState::WindowLocalSourceState(WindowGlobalSourceState &gsource) : gsource(gsource), batch_index(0) {
	auto &gsink = gsource.gsink;
	auto &global_partition = *gsink.global_partition;

	input_chunk.Initialize(global_partition.allocator, global_partition.payload_types);

	vector<LogicalType> output_types;
	for (auto &wexec : gsink.executors) {
		auto &wexpr = wexec->wexpr;
		output_types.emplace_back(wexpr.return_type);
	}
	output_chunk.Initialize(global_partition.allocator, output_types);
}

bool WindowGlobalSourceState::TryNextTask(TaskPtr &task) {
	lock_guard<mutex> task_guard(lock);
	if (next_task >= tasks.size() || stopped) {
		task = nullptr;
		return true;
	}

	//	If the next task matches the current state of its group, then we can use it
	//	Otherwise block.
	task = &tasks[next_task];

	auto &gpart = *gsink.global_partition;
	auto &window_hash_group = gpart.window_hash_groups[task->group_idx];
	auto group_stage = window_hash_group->GetStage();

	if (task->stage == group_stage) {
		++next_task;
		return true;
	}

	task = nullptr;
	return false;
}

void WindowGlobalSourceState::FinishTask(TaskPtr task) {
	if (!task) {
		return;
	}

	auto &gpart = *gsink.global_partition;
	auto &finished_hash_group = gpart.window_hash_groups[task->group_idx];
	D_ASSERT(finished_hash_group);

	if (!--finished_hash_group->tasks_remaining) {
		finished_hash_group.reset();
	}
}

void WindowLocalSourceState::FinishHashGroup(TaskPtr prev_task) {
	scanner.reset();
	local_states.clear();

	gsource.FinishTask(prev_task);
}

bool WindowLocalSourceState::GetData(DataChunk &result) {
	// Are we done with this scanner?
	if (scanner && !scanner->Remaining()) {
		scanner.reset();
		++task->begin_idx;
	}

	//	Are we done with this task?
	while (!task || task->begin_idx >= task->end_idx || task->stage != WindowGroupStage::GETDATA) {
		auto prev_task = task;
		while (!gsource.TryNextTask(task)) {
			FinishHashGroup(prev_task);
			return false;
		}

		const auto new_group = (!task || !prev_task || task->group_idx != prev_task->group_idx);
		// Release all the old group's data if we started a new group
		if (new_group) {
			FinishHashGroup(prev_task);
			BeginHashGroup();
		}

		// Are we done?
		if (!task) {
			return true;
		}

		// Process the new state
		switch (task->stage) {
		case WindowGroupStage::SINK:
			Sink();
			D_ASSERT(task->begin_idx == task->end_idx);
			continue;
		case WindowGroupStage::FINALIZE:
			Finalize();
			D_ASSERT(task->begin_idx == task->end_idx);
			continue;
		case WindowGroupStage::GETDATA:
			D_ASSERT(task->begin_idx < task->end_idx);
			break;
		default:
			throw InternalException("Invalid window source state.");
		}
	}

	D_ASSERT(window_hash_group->GetStage() == WindowGroupStage::GETDATA);

	if (!scanner) {
		scanner = window_hash_group->GetEvaluateScanner(task->begin_idx);
		batch_index = window_hash_group->batch_base + task->begin_idx;
	}

	const auto position = scanner->Scanned();
	input_chunk.Reset();
	scanner->Scan(input_chunk);

	const auto &executors = gsource.gsink.executors;
	auto &gestates = window_hash_group->gestates;
	output_chunk.Reset();
	for (idx_t expr_idx = 0; expr_idx < executors.size(); ++expr_idx) {
		auto &executor = *executors[expr_idx];
		auto &gstate = *gestates[expr_idx];
		auto &lstate = *local_states[expr_idx];
		auto &result = output_chunk.data[expr_idx];
		executor.Evaluate(position, input_chunk, result, lstate, gstate);
	}
	output_chunk.SetCardinality(input_chunk);
	output_chunk.Verify();

	idx_t out_idx = 0;
	result.SetCardinality(input_chunk);
	for (idx_t col_idx = 0; col_idx < input_chunk.ColumnCount(); col_idx++) {
		result.data[out_idx++].Reference(input_chunk.data[col_idx]);
	}
	for (idx_t col_idx = 0; col_idx < output_chunk.ColumnCount(); col_idx++) {
		result.data[out_idx++].Reference(output_chunk.data[col_idx]);
	}
	result.Verify();

	return true;
}

unique_ptr<LocalSourceState> PhysicalWindow::GetLocalSourceState(ExecutionContext &context,
                                                                 GlobalSourceState &gsource_p) const {
	auto &gsource = gsource_p.Cast<WindowGlobalSourceState>();
	return make_uniq<WindowLocalSourceState>(gsource);
}

unique_ptr<GlobalSourceState> PhysicalWindow::GetGlobalSourceState(ClientContext &context) const {
	auto &gsink = sink_state->Cast<WindowGlobalSinkState>();
	return make_uniq<WindowGlobalSourceState>(context, gsink);
}

bool PhysicalWindow::SupportsBatchIndex() const {
	//	We can only preserve order for single partitioning
	//	or work stealing causes out of order batch numbers
	auto &wexpr = select_list[order_idx]->Cast<BoundWindowExpression>();
	return wexpr.partitions.empty() && !wexpr.orders.empty();
}

OrderPreservationType PhysicalWindow::SourceOrder() const {
	return SupportsBatchIndex() ? OrderPreservationType::FIXED_ORDER : OrderPreservationType::NO_ORDER;
}

double PhysicalWindow::GetProgress(ClientContext &context, GlobalSourceState &gsource_p) const {
	auto &gsource = gsource_p.Cast<WindowGlobalSourceState>();
	const auto returned = gsource.returned.load();

	auto &gsink = gsource.gsink;
	const auto count = gsink.global_partition->count.load();
	return count ? (double(returned) / double(count)) : -1;
}

idx_t PhysicalWindow::GetBatchIndex(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate_p,
                                    LocalSourceState &lstate_p) const {
	auto &lstate = lstate_p.Cast<WindowLocalSourceState>();
	return lstate.batch_index;
}

bool WindowGlobalSourceState::UpdateBlockedTasks(bool blocked, InterruptState &interrupt_state) const {
	lock_guard<mutex> guard(lock);
	if (blocked) {
		blocked_tasks.push_back(interrupt_state);
	} else {
		// The pipeline is unblocked, so flush tasks
		for (auto &state : blocked_tasks) {
			state.Callback();
		}
		blocked_tasks.clear();
	}
	return blocked;
}

SourceResultType PhysicalWindow::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
	auto &gsource = input.global_state.Cast<WindowGlobalSourceState>();
	auto &lsource = input.local_state.Cast<WindowLocalSourceState>();

#if 1
	//	Debugging variant
	try {
		while (!lsource.GetData(chunk)) {
			TaskScheduler::GetScheduler(context.client).YieldThread();
		}
	} catch (...) {
		gsource.stopped = true;
		throw;
	}
#else
	try {
		const auto blocked = !lsource.GetData(chunk);
		if (gsource.UpdateBlockedTasks(blocked, input.interrupt_state)) {
			return SourceResultType::BLOCKED;
		}
	} catch (...) {
		gsource.stopped = true;
		gsource.UpdateBlockedTasks(false, input.interrupt_state);
		throw;
	}
#endif
	gsource.returned += chunk.size();

	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

string PhysicalWindow::ParamsToString() const {
	string result;
	for (idx_t i = 0; i < select_list.size(); i++) {
		if (i > 0) {
			result += "\n";
		}
		result += select_list[i]->GetName();
	}
	return result;
}

} // namespace duckdb
