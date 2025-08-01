#include <ranges>
#include <Access/AccessControl.h>

#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeInterval.h>

#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTOrderByElement.h>
#include <Parsers/ASTInterpolateElement.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSelectIntersectExceptQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseQuery.h>

#include <Access/Common/AccessFlags.h>
#include <Access/ContextAccess.h>

#include <AggregateFunctions/AggregateFunctionCount.h>
#include <DataTypes/DataTypeNullable.h>

#include <Interpreters/ApplyWithAliasVisitor.h>
#include <Interpreters/ApplyWithSubqueryVisitor.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/InterpreterFactory.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/InterpreterSelectWithUnionQuery.h>
#include <Interpreters/InterpreterSetQuery.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/addTypeConversionToAST.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/getTableExpressions.h>
#include <Interpreters/JoinToSubqueryTransformVisitor.h>
#include <Interpreters/CrossToInnerJoinVisitor.h>
#include <Interpreters/TableJoin.h>
#include <Interpreters/JoinedTables.h>
#include <Interpreters/OpenTelemetrySpanLog.h>
#include <Interpreters/QueryAliasesVisitor.h>
#include <Interpreters/QueryLog.h>
#include <Interpreters/replaceAliasColumnsInQuery.h>
#include <Interpreters/RewriteCountDistinctVisitor.h>
#include <Interpreters/RewriteUniqToCountVisitor.h>
#include <Interpreters/getCustomKeyFilterForParallelReplicas.h>
#include <Interpreters/Context.h>

#include <QueryPipeline/Pipe.h>
#include <Processors/QueryPlan/AggregatingStep.h>
#include <Processors/QueryPlan/ArrayJoinStep.h>
#include <Processors/QueryPlan/CreateSetAndFilterOnTheFlyStep.h>
#include <Processors/QueryPlan/CreatingSetsStep.h>
#include <Processors/QueryPlan/CubeStep.h>
#include <Processors/QueryPlan/DistinctStep.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/ExtremesStep.h>
#include <Processors/QueryPlan/FillingStep.h>
#include <Processors/QueryPlan/FilterStep.h>
#include <Processors/QueryPlan/JoinStep.h>
#include <Processors/QueryPlan/LimitByStep.h>
#include <Processors/QueryPlan/LimitStep.h>
#include <Processors/QueryPlan/SortingStep.h>
#include <Processors/QueryPlan/MergingAggregatedStep.h>
#include <Processors/QueryPlan/OffsetStep.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/ReadFromPreparedSource.h>
#include <Processors/QueryPlan/ReadNothingStep.h>
#include <Processors/QueryPlan/RollupStep.h>
#include <Processors/QueryPlan/TotalsHavingStep.h>
#include <Processors/QueryPlan/WindowStep.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Processors/Sources/NullSource.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <Processors/Transforms/AggregatingTransform.h>
#include <Processors/Transforms/FilterTransform.h>
#include <QueryPipeline/QueryPipelineBuilder.h>

#include <Storages/MergeTree/MergeTreeWhereOptimizer.h>
#include <Storages/StorageDistributed.h>
#include <Storages/StorageMerge.h>
#include <Storages/StorageValues.h>
#include <Storages/StorageView.h>
#include <Storages/ReadInOrderOptimizer.h>

#include <Columns/Collator.h>
#include <Columns/ColumnAggregateFunction.h>
#include <Core/ColumnNumbers.h>
#include <Core/Field.h>
#include <Core/ProtocolDefines.h>
#include <Core/Settings.h>
#include <Core/ServerSettings.h>
#include <Interpreters/Aggregator.h>
#include <Interpreters/ArrayJoinAction.h>
#include <Interpreters/HashTablesStatistics.h>
#include <Interpreters/IJoin.h>
#include <QueryPipeline/SizeLimits.h>
#include <Common/FieldVisitorToString.h>
#include <Common/FieldAccurateComparison.h>
#include <Common/NaNUtils.h>
#include <Common/ProfileEvents.h>
#include <Common/checkStackSize.h>
#include <Common/quoteString.h>
#include <Common/scope_guard_safe.h>
#include <Common/typeid_cast.h>


namespace ProfileEvents
{
    extern const Event SelectQueriesWithSubqueries;
    extern const Event QueriesWithSubqueries;
}

namespace DB
{
namespace Setting
{
    extern const SettingsMap additional_table_filters;
    extern const SettingsUInt64 aggregation_in_order_max_block_bytes;
    extern const SettingsUInt64 aggregation_memory_efficient_merge_threads;
    extern const SettingsUInt64 allow_experimental_parallel_reading_from_replicas;
    extern const SettingsBool allow_experimental_query_deduplication;
    extern const SettingsBool async_socket_for_remote;
    extern const SettingsBool collect_hash_table_stats_during_aggregation;
    extern const SettingsBool compile_sort_description;
    extern const SettingsBool count_distinct_optimization;
    extern const SettingsUInt64 cross_to_inner_join_rewrite;
    extern const SettingsOverflowMode distinct_overflow_mode;
    extern const SettingsBool distributed_aggregation_memory_efficient;
    extern const SettingsBool empty_result_for_aggregation_by_constant_keys_on_empty_set;
    extern const SettingsBool empty_result_for_aggregation_by_empty_set;
    extern const SettingsBool enable_global_with_statement;
    extern const SettingsBool enable_memory_bound_merging_of_aggregation_results;
    extern const SettingsBool exact_rows_before_limit;
    extern const SettingsBool enable_unaligned_array_join;
    extern const SettingsBool extremes;
    extern const SettingsBool final;
    extern const SettingsBool force_aggregation_in_order;
    extern const SettingsUInt64 group_by_two_level_threshold;
    extern const SettingsUInt64 group_by_two_level_threshold_bytes;
    extern const SettingsBool group_by_use_nulls;
    extern const SettingsSeconds lock_acquire_timeout;
    extern const SettingsUInt64 max_analyze_depth;
    extern const SettingsNonZeroUInt64 max_block_size;
    extern const SettingsUInt64 max_bytes_in_distinct;
    extern const SettingsUInt64 max_columns_to_read;
    extern const SettingsUInt64 max_distributed_connections;
    extern const SettingsNonZeroUInt64 max_parallel_replicas;
    extern const SettingsUInt64 max_parser_backtracks;
    extern const SettingsUInt64 max_parser_depth;
    extern const SettingsUInt64 max_query_size;
    extern const SettingsUInt64 max_result_bytes;
    extern const SettingsUInt64 max_result_rows;
    extern const SettingsUInt64 max_rows_in_distinct;
    extern const SettingsUInt64 max_rows_in_set_to_optimize_join;
    extern const SettingsUInt64 max_rows_to_read;
    extern const SettingsUInt64 max_size_to_preallocate_for_aggregation;
    extern const SettingsFloat max_streams_to_max_threads_ratio;
    extern const SettingsUInt64 max_subquery_depth;
    extern const SettingsMaxThreads max_threads;
    extern const SettingsUInt64 min_count_to_compile_sort_description;
    extern const SettingsBool multiple_joins_try_to_keep_original_names;
    extern const SettingsBool optimize_aggregation_in_order;
    extern const SettingsBool optimize_move_to_prewhere;
    extern const SettingsBool optimize_move_to_prewhere_if_final;
    extern const SettingsBool optimize_uniq_to_count;
    extern const SettingsUInt64 parallel_replicas_count;
    extern const SettingsString parallel_replicas_custom_key;
    extern const SettingsParallelReplicasMode parallel_replicas_mode;
    extern const SettingsUInt64 parallel_replicas_custom_key_range_lower;
    extern const SettingsUInt64 parallel_replicas_custom_key_range_upper;
    extern const SettingsBool parallel_replicas_for_non_replicated_merge_tree;
    extern const SettingsUInt64 parallel_replicas_min_number_of_rows_per_replica;
    extern const SettingsUInt64 parallel_replica_offset;
    extern const SettingsBool query_plan_enable_optimizations;
    extern const SettingsBool query_plan_enable_multithreading_after_window_functions;
    extern const SettingsBool query_plan_optimize_prewhere;
    extern const SettingsBool throw_on_unsupported_query_inside_transaction;
    extern const SettingsFloat totals_auto_threshold;
    extern const SettingsTotalsMode totals_mode;
    extern const SettingsBool use_concurrency_control;
    extern const SettingsBool use_with_fill_by_sorting_prefix;
    extern const SettingsFloat min_hit_rate_to_use_consecutive_keys_optimization;
    extern const SettingsUInt64 max_rows_to_group_by;
    extern const SettingsOverflowModeGroupBy group_by_overflow_mode;
    extern const SettingsUInt64 max_bytes_before_external_group_by;
    extern const SettingsDouble max_bytes_ratio_before_external_group_by;
    extern const SettingsUInt64 min_free_disk_space_for_temporary_data;
    extern const SettingsBool compile_aggregate_expressions;
    extern const SettingsUInt64 min_count_to_compile_aggregate_expression;
    extern const SettingsBool enable_software_prefetch_in_aggregation;
    extern const SettingsBool optimize_group_by_constant_keys;
    extern const SettingsUInt64 max_bytes_to_transfer;
    extern const SettingsUInt64 max_rows_to_transfer;
    extern const SettingsOverflowMode transfer_overflow_mode;
    extern const SettingsString implicit_table_at_top_level;
}

namespace ServerSetting
{
    extern const ServerSettingsUInt64 max_entries_for_hash_table_stats;
}

static UInt64 getLimitUIntValue(const ASTPtr & node, const ContextPtr & context, const std::string & expr);

namespace ErrorCodes
{
    extern const int TOO_DEEP_SUBQUERIES;
    extern const int SAMPLING_NOT_SUPPORTED;
    extern const int ILLEGAL_FINAL;
    extern const int ILLEGAL_PREWHERE;
    extern const int TOO_MANY_COLUMNS;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int PARAMETER_OUT_OF_BOUND;
    extern const int INVALID_LIMIT_EXPRESSION;
    extern const int INVALID_WITH_FILL_EXPRESSION;
    extern const int ACCESS_DENIED;
    extern const int UNKNOWN_IDENTIFIER;
    extern const int BAD_ARGUMENTS;
    extern const int SUPPORT_IS_DISABLED;
}

/// Assumes `storage` is set and the table filter (row-level security) is not empty.
FilterDAGInfoPtr generateFilterActions(
    const StorageID & table_id,
    const ASTPtr & row_policy_filter_expression,
    const ContextPtr & context,
    const StoragePtr & storage,
    const StorageSnapshotPtr & storage_snapshot,
    const StorageMetadataPtr & metadata_snapshot,
    Names & prerequisite_columns,
    PreparedSetsPtr prepared_sets)
try
{
    auto filter_info = std::make_shared<FilterDAGInfo>();

    const auto & db_name = table_id.getDatabaseName();
    const auto & table_name = table_id.getTableName();

    /// TODO: implement some AST builders for this kind of stuff
    ASTPtr query_ast = std::make_shared<ASTSelectQuery>();
    auto * select_ast = query_ast->as<ASTSelectQuery>();

    select_ast->setExpression(ASTSelectQuery::Expression::SELECT, std::make_shared<ASTExpressionList>());
    auto expr_list = select_ast->select();

    /// The first column is our filter expression.
    /// the row_policy_filter_expression should be cloned, because it may be changed by TreeRewriter.
    /// which make it possible an invalid expression, although it may be valid in whole select.
    expr_list->children.push_back(row_policy_filter_expression->clone());

    /// Keep columns that are required after the filter actions.
    for (const auto & column_str : prerequisite_columns)
    {
        ParserExpression expr_parser;
        /// We should add back quotes around column name as it can contain dots.
        expr_list->children.push_back(parseQuery(
            expr_parser,
            backQuoteIfNeed(column_str),
            0,
            context->getSettingsRef()[Setting::max_parser_depth],
            context->getSettingsRef()[Setting::max_parser_backtracks]));
    }

    select_ast->setExpression(ASTSelectQuery::Expression::TABLES, std::make_shared<ASTTablesInSelectQuery>());
    auto tables = select_ast->tables();
    auto tables_elem = std::make_shared<ASTTablesInSelectQueryElement>();
    auto table_expr = std::make_shared<ASTTableExpression>();
    tables->children.push_back(tables_elem);
    tables_elem->table_expression = table_expr;
    tables_elem->children.push_back(table_expr);
    table_expr->database_and_table_name = std::make_shared<ASTTableIdentifier>(db_name, table_name);
    table_expr->children.push_back(table_expr->database_and_table_name);

    /// Using separate expression analyzer to prevent any possible alias injection
    auto syntax_result = TreeRewriter(context).analyzeSelect(query_ast, TreeRewriterResult({}, storage, storage_snapshot));
    SelectQueryExpressionAnalyzer analyzer(query_ast, syntax_result, context, metadata_snapshot, {}, false, {}, prepared_sets);
    filter_info->actions = std::move(analyzer.simpleSelectActions()->dag);

    filter_info->column_name = expr_list->children.at(0)->getColumnName();
    filter_info->actions.removeUnusedActions(NameSet{filter_info->column_name});

    for (const auto * node : filter_info->actions.getInputs())
        filter_info->actions.getOutputs().push_back(node);

    auto required_columns_from_filter = filter_info->actions.getRequiredColumns();

    for (const auto & column : required_columns_from_filter)
    {
        if (prerequisite_columns.end() == std::find(prerequisite_columns.begin(), prerequisite_columns.end(), column.name))
            prerequisite_columns.push_back(column.name);
    }

    return filter_info;
}
catch (Exception & e)
{
    e.addMessage("While applying a row policy (see system.row_policies)");
    throw;
}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const ContextPtr & context_,
    const SelectQueryOptions & options_,
    const Names & required_result_column_names_)
    : InterpreterSelectQuery(query_ptr_, context_, std::nullopt, nullptr, options_, required_result_column_names_)
{}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const ContextMutablePtr & context_,
    const SelectQueryOptions & options_,
    const Names & required_result_column_names_)
    : InterpreterSelectQuery(query_ptr_, context_, std::nullopt, nullptr, options_, required_result_column_names_)
{}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const ContextPtr & context_,
    Pipe input_pipe_,
    const SelectQueryOptions & options_)
    : InterpreterSelectQuery(query_ptr_, context_, std::move(input_pipe_), nullptr, options_.copy().noSubquery())
{}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const ContextPtr & context_,
    const StoragePtr & storage_,
    const StorageMetadataPtr & metadata_snapshot_,
    const SelectQueryOptions & options_)
    : InterpreterSelectQuery(query_ptr_, context_, std::nullopt, storage_, options_.copy().noSubquery(), {}, metadata_snapshot_)
{}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const ContextPtr & context_,
    const SelectQueryOptions & options_,
    PreparedSetsPtr prepared_sets_)
    : InterpreterSelectQuery(query_ptr_, context_, std::nullopt, nullptr, options_, {}, {}, prepared_sets_)
{}

InterpreterSelectQuery::~InterpreterSelectQuery() = default;


namespace
{

/** There are no limits on the maximum size of the result for the subquery.
  *  Since the result of the query is not the result of the entire query.
  */
ContextPtr getSubqueryContext(const ContextPtr & context)
{
    auto subquery_context = Context::createCopy(context);
    Settings subquery_settings = context->getSettingsCopy();
    subquery_settings[Setting::max_result_rows] = 0;
    subquery_settings[Setting::max_result_bytes] = 0;
    subquery_settings[Setting::implicit_table_at_top_level] = "";
    /// The calculation of extremes does not make sense and is not necessary (if you do it, then the extremes of the subquery can be taken for whole query).
    subquery_settings[Setting::extremes] = false;
    subquery_context->setSettings(subquery_settings);
    return subquery_context;
}

void rewriteMultipleJoins(ASTPtr & query, const TablesWithColumns & tables, const String & database, const Settings & settings)
{
    ASTSelectQuery & select = query->as<ASTSelectQuery &>();

    Aliases aliases;
    if (ASTPtr with = select.with())
        QueryAliasesNoSubqueriesVisitor(aliases).visit(with);
    QueryAliasesNoSubqueriesVisitor(aliases).visit(select.select());

    CrossToInnerJoinVisitor::Data cross_to_inner{tables, aliases, database};
    cross_to_inner.cross_to_inner_join_rewrite = static_cast<UInt8>(std::min<UInt64>(settings[Setting::cross_to_inner_join_rewrite], 2));
    CrossToInnerJoinVisitor(cross_to_inner).visit(query);

    JoinToSubqueryTransformVisitor::Data join_to_subs_data{tables, aliases};
    join_to_subs_data.try_to_keep_original_names = settings[Setting::multiple_joins_try_to_keep_original_names];

    JoinToSubqueryTransformVisitor(join_to_subs_data).visit(query);
}

/// Checks that the current user has the SELECT privilege.
void checkAccessRightsForSelect(
    const ContextPtr & context,
    const StorageID & table_id,
    const StorageMetadataPtr & table_metadata,
    const TreeRewriterResult & syntax_analyzer_result)
{
    if (!syntax_analyzer_result.has_explicit_columns && table_metadata && !table_metadata->getColumns().empty())
    {
        /// For a trivial query like "SELECT count() FROM table" access is granted if at least
        /// one column is accessible.
        /// In this case just checking access for `required_columns` doesn't work correctly
        /// because `required_columns` will contain the name of a column of minimum size (see TreeRewriterResult::collectUsedColumns())
        /// which is probably not the same column as the column the current user has access to.
        auto access = context->getAccess();
        for (const auto & column : table_metadata->getColumns())
        {
            if (access->isGranted(AccessType::SELECT, table_id.database_name, table_id.table_name, column.name))
                return;
        }
        throw Exception(
            ErrorCodes::ACCESS_DENIED,
            "{}: Not enough privileges. To execute this query, it's necessary to have the grant SELECT for at least one column on {}",
            context->getUserName(),
            table_id.getFullTableName());
    }

    /// General check.
    context->checkAccess(AccessType::SELECT, table_id, syntax_analyzer_result.requiredSourceColumnsForAccessCheck());
}

ASTPtr parseAdditionalFilterConditionForTable(
    const Map & additional_table_filters,
    const DatabaseAndTableWithAlias & target,
    const Context & context)
{
    for (const auto & additional_filter : additional_table_filters)
    {
        const auto & tuple = additional_filter.safeGet<Tuple>();
        const auto & table = tuple.at(0).safeGet<String>();
        const auto & filter = tuple.at(1).safeGet<String>();

        if (table == target.alias ||
            (table == target.table && context.getCurrentDatabase() == target.database) ||
            (table == target.database + '.' + target.table))
        {
            /// Try to parse expression
            ParserExpression parser;
            const auto & settings = context.getSettingsRef();
            return parseQuery(
                parser,
                filter.data(),
                filter.data() + filter.size(),
                "additional filter",
                settings[Setting::max_query_size],
                settings[Setting::max_parser_depth],
                settings[Setting::max_parser_backtracks]);
        }
    }

    return nullptr;
}

/// Returns true if we should ignore quotas and limits for a specified table in the system database.
bool shouldIgnoreQuotaAndLimits(const StorageID & table_id)
{
    if (table_id.database_name == DatabaseCatalog::SYSTEM_DATABASE)
    {
        static const boost::container::flat_set<String> tables_ignoring_quota{"quotas", "quota_limits", "quota_usage", "quotas_usage", "one"};
        if (tables_ignoring_quota.count(table_id.table_name))
            return true;
    }
    return false;
}

GroupingSetsParamsList getAggregatorGroupingSetsParams(const NamesAndTypesLists & aggregation_keys_list, const Names & all_keys)
{
    GroupingSetsParamsList result;

    for (const auto & aggregation_keys : aggregation_keys_list)
    {
        NameSet keys;
        for (const auto & key : aggregation_keys)
            keys.insert(key.name);

        Names missing_keys;
        for (const auto & key : all_keys)
            if (!keys.contains(key))
                missing_keys.push_back(key);

        result.emplace_back(aggregation_keys.getNames(), std::move(missing_keys));
    }

    return result;
}

}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const ContextPtr & context_,
    std::optional<Pipe> input_pipe_,
    const StoragePtr & storage_,
    const SelectQueryOptions & options_,
    const Names & required_result_column_names,
    const StorageMetadataPtr & metadata_snapshot_,
    PreparedSetsPtr prepared_sets_)
    : InterpreterSelectQuery(
        query_ptr_,
        Context::createCopy(context_),
        std::move(input_pipe_),
        storage_,
        options_,
        required_result_column_names,
        metadata_snapshot_,
        prepared_sets_)
{}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const ContextMutablePtr & context_,
    std::optional<Pipe> input_pipe_,
    const StoragePtr & storage_,
    const SelectQueryOptions & options_,
    const Names & required_result_column_names,
    const StorageMetadataPtr & metadata_snapshot_,
    PreparedSetsPtr prepared_sets_)
    /// NOTE: the query almost always should be cloned because it will be modified during analysis.
    : IInterpreterUnionOrSelectQuery(options_.modify_inplace ? query_ptr_ : query_ptr_->clone(), context_, options_)
    , source_header(std::make_shared<const Block>(Block{}))
    , storage(storage_)
    , input_pipe(std::move(input_pipe_))
    , log(getLogger("InterpreterSelectQuery"))
    , metadata_snapshot(metadata_snapshot_)
    , prepared_sets(prepared_sets_)
{
    checkStackSize();

    if (!prepared_sets)
        prepared_sets = std::make_shared<PreparedSets>();

    query_info.is_internal = options.is_internal;

    initSettings();
    const Settings & settings = context->getSettingsRef();

    if (settings[Setting::max_subquery_depth] && options.subquery_depth > settings[Setting::max_subquery_depth])
        throw Exception(ErrorCodes::TOO_DEEP_SUBQUERIES, "Too deep subqueries. Maximum: {}", settings[Setting::max_subquery_depth].toString());

    bool has_input = input_pipe != std::nullopt;
    if (input_pipe)
    {
        /// Read from prepared input.
        source_header = input_pipe->getSharedHeader();
    }

    // Only propagate WITH elements to subqueries if we're not a subquery
    if (!options.is_subquery)
    {
        if (context->getSettingsRef()[Setting::enable_global_with_statement])
            ApplyWithAliasVisitor::visit(query_ptr);
        ApplyWithSubqueryVisitor(context).visit(query_ptr);
    }

    query_info.query = query_ptr->clone();

    if (settings[Setting::count_distinct_optimization])
    {
        RewriteCountDistinctFunctionMatcher::Data data_rewrite_countdistinct;
        RewriteCountDistinctFunctionVisitor(data_rewrite_countdistinct).visit(query_ptr);
    }

    if (settings[Setting::optimize_uniq_to_count])
    {
        RewriteUniqToCountMatcher::Data data_rewrite_uniq_count;
        RewriteUniqToCountVisitor(data_rewrite_uniq_count).visit(query_ptr);
    }

    JoinedTables joined_tables(getSubqueryContext(context), getSelectQuery(), options.with_all_cols, options_.is_create_parameterized_view);

    bool got_storage_from_query = false;
    if (!has_input && !storage)
    {
        storage = joined_tables.getLeftTableStorage();
        // Mark uses_view_source if the returned storage is the same as the one saved in viewSource
        uses_view_source |= storage && storage == context->getViewSource();
        got_storage_from_query = true;
    }

    if (storage)
    {
        if (storage->updateExternalDynamicMetadataIfExists(context))
        {
            metadata_snapshot = storage->getInMemoryMetadataPtr();
        }

        table_lock = storage->lockForShare(context->getInitialQueryId(), context->getSettingsRef()[Setting::lock_acquire_timeout]);
        table_id = storage->getStorageID();

        if (!metadata_snapshot)
            metadata_snapshot = storage->getInMemoryMetadataPtr();

        if (options.only_analyze)
            storage_snapshot = storage->getStorageSnapshotWithoutData(metadata_snapshot, context);
        else
            storage_snapshot = storage->getStorageSnapshotForQuery(metadata_snapshot, query_ptr, context);
    }

    if (has_input || !joined_tables.resolveTables())
        joined_tables.makeFakeTable(storage, metadata_snapshot, *source_header);

    if (context->getCurrentTransaction() && context->getSettingsRef()[Setting::throw_on_unsupported_query_inside_transaction])
    {
        if (storage)
            checkStorageSupportsTransactionsIfNeeded(storage, context, /* is_readonly_query */ true);
        for (const auto & table : joined_tables.tablesWithColumns())
        {
            if (table.table.table.empty())
                continue;
            auto maybe_storage = DatabaseCatalog::instance().tryGetTable({table.table.database, table.table.table}, context);
            if (!maybe_storage)
                continue;
            checkStorageSupportsTransactionsIfNeeded(maybe_storage, context, /* is_readonly_query */ true);
        }
    }

    /// Check support for JOIN for parallel replicas with custom key
    if (joined_tables.tablesCount() > 1 && !settings[Setting::parallel_replicas_custom_key].value.empty())
    {
        LOG_DEBUG(log, "JOINs are not supported with parallel_replicas_custom_key. Query will be executed without using them.");
        context->setSetting("parallel_replicas_custom_key", String{""});
    }

    /// Check support for FINAL for parallel replicas
    bool is_query_with_final = isQueryWithFinal(query_info);
    if (is_query_with_final && context->canUseTaskBasedParallelReplicas())
    {
        if (settings[Setting::allow_experimental_parallel_reading_from_replicas] == 1)
        {
            LOG_DEBUG(log, "FINAL modifier is not supported with parallel replicas. Query will be executed without using them.");
            context->setSetting("allow_experimental_parallel_reading_from_replicas", Field(0));
        }
        else if (settings[Setting::allow_experimental_parallel_reading_from_replicas] >= 2)
        {
            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "FINAL modifier is not supported with parallel replicas");
        }
    }

    /// Check support for parallel replicas for non-replicated storage (plain MergeTree)
    bool is_plain_merge_tree = storage && storage->isMergeTree() && !storage->supportsReplication();
    if (is_plain_merge_tree && settings[Setting::allow_experimental_parallel_reading_from_replicas] > 0
        && !settings[Setting::parallel_replicas_for_non_replicated_merge_tree])
    {
        if (settings[Setting::allow_experimental_parallel_reading_from_replicas] == 1)
        {
            LOG_DEBUG(log, "To use parallel replicas with plain MergeTree tables please enable setting `parallel_replicas_for_non_replicated_merge_tree`. For now query will be executed without using them.");
            context->setSetting("allow_experimental_parallel_reading_from_replicas", Field(0));
        }
        else if (settings[Setting::allow_experimental_parallel_reading_from_replicas] >= 2)
        {
            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "To use parallel replicas with plain MergeTree tables please enable setting `parallel_replicas_for_non_replicated_merge_tree`");
        }
    }

    /// Rewrite JOINs
    if (!has_input && joined_tables.tablesCount() > 1)
    {
        rewriteMultipleJoins(query_ptr, joined_tables.tablesWithColumns(), context->getCurrentDatabase(), context->getSettingsRef());

        joined_tables.reset(getSelectQuery());
        joined_tables.resolveTables();
        if (auto view_source = context->getViewSource())
        {
            // If we are using a virtual block view to replace a table and that table is used
            // inside the JOIN then we need to update uses_view_source accordingly so we avoid propagating scalars that we can't cache
            const auto & storage_values = static_cast<const StorageValues &>(*view_source);
            auto tmp_table_id = storage_values.getStorageID();
            for (const auto & t : joined_tables.tablesWithColumns())
                uses_view_source |= (t.table.database == tmp_table_id.database_name && t.table.table == tmp_table_id.table_name);
        }

        if (storage && joined_tables.isLeftTableSubquery())
        {
            /// Rewritten with subquery. Free storage locks here.
            storage = nullptr;
            table_lock.reset();
            table_id = StorageID::createEmpty();
            metadata_snapshot = nullptr;
            storage_snapshot = nullptr;
        }
    }

    if (!has_input)
    {
        interpreter_subquery = joined_tables.makeLeftTableSubquery(options.subquery());
        if (interpreter_subquery)
        {
            source_header = interpreter_subquery->getSampleBlock();
            uses_view_source |= interpreter_subquery->usesViewSource();
        }
    }

    joined_tables.rewriteDistributedInAndJoins(query_ptr);

    max_streams = settings[Setting::max_threads];
    ASTSelectQuery & query = getSelectQuery();
    std::shared_ptr<TableJoin> table_join = joined_tables.makeTableJoin(query);

    if (storage)
        row_policy_filter = context->getRowPolicyFilter(table_id.getDatabaseName(), table_id.getTableName(), RowPolicyFilterType::SELECT_FILTER);

    StorageView * view = nullptr;
    if (storage)
        view = dynamic_cast<StorageView *>(storage.get());

    if (!settings[Setting::additional_table_filters].value.empty() && storage && !joined_tables.tablesWithColumns().empty())
        query_info.additional_filter_ast = parseAdditionalFilterConditionForTable(
            settings[Setting::additional_table_filters], joined_tables.tablesWithColumns().front().table, *context);

    ASTPtr parallel_replicas_custom_filter_ast = nullptr;
    if (storage && context->canUseParallelReplicasCustomKey() && !joined_tables.tablesWithColumns().empty())
    {
        if (settings[Setting::parallel_replicas_count] > 1)
        {
            if (auto custom_key_ast = parseCustomKeyForTable(settings[Setting::parallel_replicas_custom_key], *context))
            {
                LOG_TRACE(log, "Processing query on a replica using custom_key '{}'", settings[Setting::parallel_replicas_custom_key].value);

                parallel_replicas_custom_filter_ast = getCustomKeyFilterForParallelReplica(
                    settings[Setting::parallel_replicas_count],
                    settings[Setting::parallel_replica_offset],
                    std::move(custom_key_ast),
                    {settings[Setting::parallel_replicas_mode],
                     settings[Setting::parallel_replicas_custom_key_range_lower],
                     settings[Setting::parallel_replicas_custom_key_range_upper]},
                    storage->getInMemoryMetadataPtr()->columns,
                    context);
            }
            else if (settings[Setting::parallel_replica_offset] > 0)
            {
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Parallel replicas processing with custom_key has been requested "
                    "(setting 'max_parallel_replicas') but the table does not have custom_key defined for it "
                    "or it's invalid (settings `parallel_replicas_custom_key`)");
            }
        }
        /// We disable prefer_localhost_replica because if one of the replicas is local it will create a single local plan
        /// instead of executing the query with multiple replicas
        /// We can enable this setting again for custom key parallel replicas when we can generate a plan that will use both a
        /// local plan and remote replicas
        else if (auto * distributed = dynamic_cast<StorageDistributed *>(storage.get());
                 distributed && context->canUseParallelReplicasCustomKeyForCluster(*distributed->getCluster()))
        {
            context->setSetting("distributed_group_by_no_merge", 2);
            context->setSetting("prefer_localhost_replica", Field(0));
        }
        else if (
            storage->isMergeTree() && (storage->supportsReplication() || settings[Setting::parallel_replicas_for_non_replicated_merge_tree])
            && context->getClientInfo().distributed_depth == 0
            && context->canUseParallelReplicasCustomKeyForCluster(*context->getClusterForParallelReplicas()))
        {
            context->setSetting("prefer_localhost_replica", Field(0));
        }
    }

    if (autoFinalOnQuery(query))
    {
        query.setFinal();
    }

    auto analyze = [&] (bool try_move_to_prewhere)
    {
        /// Allow push down and other optimizations for VIEW: replace with subquery and rewrite it.
        ASTPtr view_table;
        if (view)
        {
            query_info.is_parameterized_view = view->isParameterizedView();
            StorageView::replaceWithSubquery(getSelectQuery(), view_table, metadata_snapshot, view->isParameterizedView());
        }

        syntax_analyzer_result = TreeRewriter(context).analyzeSelect(
            query_ptr,
            TreeRewriterResult(source_header->getNamesAndTypesList(), storage, storage_snapshot),
            options,
            joined_tables.tablesWithColumns(),
            required_result_column_names,
            table_join);

        query_info.syntax_analyzer_result = syntax_analyzer_result;
        context->setDistributed(syntax_analyzer_result->is_remote_storage);

        if (storage && !query.final() && storage->needRewriteQueryWithFinal(syntax_analyzer_result->requiredSourceColumns()))
            query.setFinal();

        if (view)
        {
            /// Restore original view name. Save rewritten subquery for future usage in StorageView.
            query_info.view_query = StorageView::restoreViewName(getSelectQuery(), view_table);
            view = nullptr;
        }

        if (try_move_to_prewhere
            && storage && storage->canMoveConditionsToPrewhere()
            && query.where() && !query.prewhere()
            && !query.hasJoin()) /// Join may produce rows with nulls or default values, it's difficult to analyze if they affected or not.
        {
            /// PREWHERE optimization: transfer some condition from WHERE to PREWHERE if enabled and viable
            if (const auto & column_sizes = storage->getColumnSizes(); !column_sizes.empty())
            {
                /// Extract column compressed sizes.
                std::unordered_map<std::string, UInt64> column_compressed_sizes;
                for (const auto & [name, sizes] : column_sizes)
                    column_compressed_sizes[name] = sizes.data_compressed;

                SelectQueryInfo current_info;
                current_info.query = query_ptr;
                current_info.syntax_analyzer_result = syntax_analyzer_result;

                Names queried_columns = syntax_analyzer_result->requiredSourceColumns();
                const auto & supported_prewhere_columns = storage->supportedPrewhereColumns();

                MergeTreeWhereOptimizer where_optimizer{
                    std::move(column_compressed_sizes),
                    metadata_snapshot,
                    storage->getConditionSelectivityEstimatorByPredicate(storage_snapshot, nullptr, context),
                    queried_columns,
                    supported_prewhere_columns,
                    log};

                where_optimizer.optimize(current_info, context);
            }
        }

        if (query.prewhere() && query.where())
        {
            /// Filter block in WHERE instead to get better performance
            query.setExpression(
                ASTSelectQuery::Expression::WHERE, makeASTFunction("and", query.prewhere()->clone(), query.where()->clone()));
        }

        query_analyzer = std::make_unique<SelectQueryExpressionAnalyzer>(
            query_ptr,
            syntax_analyzer_result,
            context,
            metadata_snapshot,
            required_result_column_names,
            !options.only_analyze,
            options,
            prepared_sets);

        if (!options.only_analyze)
        {
            if (query.sampleSize() && (input_pipe || !storage || !storage->supportsSampling()))
            {
                if (storage)
                    throw Exception(
                        ErrorCodes::SAMPLING_NOT_SUPPORTED,
                        "Storage {} doesn't support sampling",
                        storage->getStorageID().getNameForLogs());
                throw Exception(
                    ErrorCodes::SAMPLING_NOT_SUPPORTED, "Illegal SAMPLE: sampling is only allowed with the table engines that support it");
            }

            if (query.final() && (input_pipe || !storage || !storage->supportsFinal()))
            {
                if (!input_pipe && storage)
                    throw Exception(ErrorCodes::ILLEGAL_FINAL, "Storage {} doesn't support FINAL", storage->getName());
                throw Exception(ErrorCodes::ILLEGAL_FINAL, "Illegal FINAL");
            }

            if (query.prewhere() && (input_pipe || !storage || !storage->supportsPrewhere()))
            {
                if (!input_pipe && storage)
                    throw Exception(ErrorCodes::ILLEGAL_PREWHERE, "Storage {} doesn't support PREWHERE", storage->getName());
                throw Exception(ErrorCodes::ILLEGAL_PREWHERE, "Illegal PREWHERE");
            }

            /// Save the new temporary tables in the query context
            for (const auto & it : query_analyzer->getExternalTables())
                if (!context->tryResolveStorageID({"", it.first}, Context::ResolveExternal))
                    context->addExternalTable(it.first, std::move(*it.second));
        }

        if (!options.only_analyze || options.modify_inplace)
        {
            if (syntax_analyzer_result->rewrite_subqueries)
            {
                /// remake interpreter_subquery when PredicateOptimizer rewrites subqueries and main table is subquery
                interpreter_subquery = joined_tables.makeLeftTableSubquery(options.subquery());
            }
        }

        if (interpreter_subquery)
        {
            /// If there is an aggregation in the outer query, WITH TOTALS is ignored in the subquery.
            if (query_analyzer->hasAggregation())
                interpreter_subquery->ignoreWithTotals();
            uses_view_source |= interpreter_subquery->usesViewSource();
        }

        required_columns = syntax_analyzer_result->requiredSourceColumns();

        if (storage)
        {
            query_info.filter_asts.clear();

            /// Fix source_header for filter actions.
            if (row_policy_filter && !row_policy_filter->empty())
            {
                filter_info = generateFilterActions(
                    table_id, row_policy_filter->expression, context, storage, storage_snapshot, metadata_snapshot, required_columns,
                    prepared_sets);

                query_info.filter_asts.push_back(row_policy_filter->expression);
            }

            if (query_info.additional_filter_ast)
            {
                additional_filter_info = generateFilterActions(
                    table_id, query_info.additional_filter_ast, context, storage, storage_snapshot, metadata_snapshot, required_columns,
                    prepared_sets);

                additional_filter_info->do_remove_column = true;

                query_info.filter_asts.push_back(query_info.additional_filter_ast);
            }

            if (parallel_replicas_custom_filter_ast)
            {
                parallel_replicas_custom_filter_info = generateFilterActions(
                        table_id, parallel_replicas_custom_filter_ast, context, storage, storage_snapshot, metadata_snapshot, required_columns,
                        prepared_sets);

                parallel_replicas_custom_filter_info->do_remove_column = true;
                query_info.filter_asts.push_back(parallel_replicas_custom_filter_ast);
            }

            source_header = std::make_shared<const Block>(storage_snapshot->getSampleBlockForColumns(required_columns));
        }

        /// Calculate structure of the result.
        result_header = std::make_shared<const Block>(getSampleBlockImpl());
    };


    /// This is a hack to make sure we reanalyze if GlobalSubqueriesVisitor changed allow_experimental_parallel_reading_from_replicas
    /// inside the query context (because it doesn't have write access to the main context)
    UInt64 parallel_replicas_before_analysis
        = context->hasQueryContext() ? context->getQueryContext()->getSettingsRef()[Setting::allow_experimental_parallel_reading_from_replicas] : 0;

    /// Conditionally support AST-based PREWHERE optimization.
    analyze(shouldMoveToPrewhere() && (!settings[Setting::query_plan_optimize_prewhere] || !settings[Setting::query_plan_enable_optimizations]));


    bool need_analyze_again = false;
    bool can_analyze_again = false;

    if (context->hasQueryContext())
    {
        /// As this query can't be executed with parallel replicas, we must reanalyze it
        if (context->getQueryContext()->getSettingsRef()[Setting::allow_experimental_parallel_reading_from_replicas]
            != parallel_replicas_before_analysis)
        {
            context->setSetting("allow_experimental_parallel_reading_from_replicas", Field(0));
            context->setSetting("max_parallel_replicas", UInt64{1});
            need_analyze_again = true;
        }

        /// Check number of calls of 'analyze' function.
        /// If it is too big, we will not analyze the query again not to have exponential blowup.
        std::atomic<size_t> & current_query_analyze_count = context->getQueryContext()->kitchen_sink.analyze_counter;
        ++current_query_analyze_count;
        can_analyze_again = settings[Setting::max_analyze_depth] == 0 || current_query_analyze_count < settings[Setting::max_analyze_depth];
    }

    if (can_analyze_again && (analysis_result.prewhere_constant_filter_description.always_false ||
                              analysis_result.prewhere_constant_filter_description.always_true))
    {
        if (analysis_result.prewhere_constant_filter_description.always_true)
            query.setExpression(ASTSelectQuery::Expression::PREWHERE, {});
        else
            query.setExpression(ASTSelectQuery::Expression::PREWHERE, std::make_shared<ASTLiteral>(0u));
        need_analyze_again = true;
    }

    if (can_analyze_again && (analysis_result.where_constant_filter_description.always_false ||
                              analysis_result.where_constant_filter_description.always_true))
    {
        if (analysis_result.where_constant_filter_description.always_true)
            query.setExpression(ASTSelectQuery::Expression::WHERE, {});
        else
            query.setExpression(ASTSelectQuery::Expression::WHERE, std::make_shared<ASTLiteral>(0u));
        need_analyze_again = true;
    }

    if (can_analyze_again)
        need_analyze_again |= adjustParallelReplicasAfterAnalysis();

    if (need_analyze_again)
    {
        size_t current_query_analyze_count = context->getQueryContext()->kitchen_sink.analyze_counter.load();
        LOG_TRACE(log, "Running 'analyze' second time (current analyze depth: {})", current_query_analyze_count);

        /// Reuse already built sets for multiple passes of analysis
        prepared_sets = query_analyzer->getPreparedSets();

        /// Do not try move conditions to PREWHERE for the second time.
        /// Otherwise, we won't be able to fallback from inefficient PREWHERE to WHERE later.
        analyze(/* try_move_to_prewhere = */ false);
    }

    /// If there is no WHERE, filter blocks as usual
    if (query.prewhere() && !query.where())
        analysis_result.prewhere_info->need_filter = true;

    if (table_id && got_storage_from_query && !joined_tables.isLeftTableFunction() && !options.ignore_access_check)
    {
        /// The current user should have the SELECT privilege. If this table_id is for a table
        /// function we don't check access rights here because in this case they have been already
        /// checked in ITableFunction::execute().
        checkAccessRightsForSelect(context, table_id, metadata_snapshot, *syntax_analyzer_result);

        /// Remove limits for some tables in the `system` database.
        if (shouldIgnoreQuotaAndLimits(table_id) && (joined_tables.tablesCount() <= 1))
        {
            options.ignore_quota = true;
            options.ignore_limits = true;
        }
    }

    /// Add prewhere actions with alias columns and record needed columns from storage.
    if (storage)
    {
        addPrewhereAliasActions();
        analysis_result.required_columns = required_columns;
    }

    /// Blocks used in expression analysis contains size 1 const columns for constant folding and
    ///  null non-const columns to avoid useless memory allocations. However, a valid block sample
    ///  requires all columns to be of size 0, thus we need to sanitize the block here.
    auto header = *result_header;
    sanitizeBlock(header, true);
    result_header = std::make_shared<const Block>(std::move(header));
}

bool InterpreterSelectQuery::adjustParallelReplicasAfterAnalysis()
{
    const Settings & settings = context->getSettingsRef();
    ASTSelectQuery & query = getSelectQuery();

    /// While only_analyze we don't know anything about parts, so any decision about how many parallel replicas to use would be wrong
    if (!storage || options.only_analyze || !context->canUseParallelReplicasOnInitiator())
        return false;

    if (getTrivialCount(0).has_value())
    {
        /// The query could use trivial count if it didn't use parallel replicas, so let's disable it and reanalyze
        context->setSetting("allow_experimental_parallel_reading_from_replicas", Field(0));
        context->setSetting("max_parallel_replicas", UInt64{1});
        LOG_DEBUG(log, "Disabling parallel replicas to be able to use a trivial count optimization");
        return true;
    }

    auto storage_merge_tree = std::dynamic_pointer_cast<MergeTreeData>(storage);
    if (!storage_merge_tree || settings[Setting::parallel_replicas_min_number_of_rows_per_replica] == 0)
        return false;

    auto query_info_copy = query_info;
    auto analysis_copy = analysis_result;

    /// There is a couple of instances where there might be a lower limit on the rows to be read
    /// * The max_rows_to_read setting
    /// * A LIMIT in a simple query (see maxBlockSizeByLimit())
    UInt64 max_rows = maxBlockSizeByLimit();
    if (settings[Setting::max_rows_to_read])
        max_rows = max_rows ? std::min(max_rows, settings[Setting::max_rows_to_read].value) : settings[Setting::max_rows_to_read];
    query_info_copy.trivial_limit = max_rows;

    /// Apply filters to prewhere and add them to the query_info so we can filter out parts efficiently during row estimation
    applyFiltersToPrewhereInAnalysis(analysis_copy);
    if (analysis_copy.prewhere_info)
    {
        query_info_copy.prewhere_info = analysis_copy.prewhere_info;
        if (query.prewhere() && !query.where())
            query_info_copy.prewhere_info->need_filter = true;
    }

    ActionDAGNodes added_filter_nodes = MergeTreeData::getFiltersForPrimaryKeyAnalysis(*this);
    if (query_info_copy.prewhere_info)
    {
        {
            const auto & node
                = query_info_copy.prewhere_info->prewhere_actions.findInOutputs(query_info_copy.prewhere_info->prewhere_column_name);
            added_filter_nodes.nodes.push_back(&node);
        }

        if (query_info_copy.prewhere_info->row_level_filter)
        {
            const auto & node
                = query_info_copy.prewhere_info->row_level_filter->findInOutputs(query_info_copy.prewhere_info->row_level_column_name);
            added_filter_nodes.nodes.push_back(&node);
        }
    }

    if (auto filter_actions_dag = ActionsDAG::buildFilterActionsDAG(added_filter_nodes.nodes))
        query_info_copy.filter_actions_dag = std::make_shared<const ActionsDAG>(std::move(*filter_actions_dag));
    UInt64 rows_to_read = storage_merge_tree->estimateNumberOfRowsToRead(context, storage_snapshot, query_info_copy);
    /// Note that we treat an estimation of 0 rows as a real estimation
    size_t number_of_replicas_to_use = rows_to_read / settings[Setting::parallel_replicas_min_number_of_rows_per_replica];
    LOG_TRACE(log, "Estimated {} rows to read. It is enough work for {} parallel replicas", rows_to_read, number_of_replicas_to_use);

    if (number_of_replicas_to_use <= 1)
    {
        context->setSetting("allow_experimental_parallel_reading_from_replicas", Field(0));
        context->setSetting("max_parallel_replicas", UInt64{1});
        LOG_DEBUG(log, "Disabling parallel replicas because there aren't enough rows to read");
        return true;
    }
    if (number_of_replicas_to_use < settings[Setting::max_parallel_replicas])
    {
        context->setSetting("max_parallel_replicas", number_of_replicas_to_use);
        LOG_DEBUG(log, "Reducing the number of replicas to use to {}", number_of_replicas_to_use);
        /// No need to reanalyze
    }
    return false;
}

void InterpreterSelectQuery::buildQueryPlan(QueryPlan & query_plan)
{
    executeImpl(query_plan, std::move(input_pipe));

    /// We must guarantee that result structure is the same as in getSampleBlock()
    if (!blocksHaveEqualStructure(*query_plan.getCurrentHeader(), *result_header))
    {
        auto convert_actions_dag = ActionsDAG::makeConvertingActions(
            query_plan.getCurrentHeader()->getColumnsWithTypeAndName(),
            result_header->getColumnsWithTypeAndName(),
            ActionsDAG::MatchColumnsMode::Name,
            true);

        auto converting = std::make_unique<ExpressionStep>(query_plan.getCurrentHeader(), std::move(convert_actions_dag));
        query_plan.addStep(std::move(converting));
    }

    /// Extend lifetime of context, table lock, storage.
    query_plan.addInterpreterContext(context);
    if (table_lock)
        query_plan.addTableLock(std::move(table_lock));
    if (storage)
        query_plan.addStorageHolder(storage);
}

BlockIO InterpreterSelectQuery::execute()
{
    BlockIO res;
    QueryPlan query_plan;

    buildQueryPlan(query_plan);

    auto builder = query_plan.buildQueryPipeline(QueryPlanOptimizationSettings(context), BuildQueryPipelineSettings(context));

    res.pipeline = QueryPipelineBuilder::getPipeline(std::move(*builder));

    setQuota(res.pipeline);

    return res;
}

Block InterpreterSelectQuery::getSampleBlockImpl()
{
    auto & select_query = getSelectQuery();

    query_info.query = query_ptr;

    /// NOTE: this is required for getQueryProcessingStage(), so should be initialized before ExpressionAnalysisResult.
    query_info.has_window = query_analyzer->hasWindow();
    /// NOTE: this is required only for IStorage::read(), and to be precise MergeTreeData::read(), in case of projections.
    query_info.has_order_by = select_query.orderBy() != nullptr;
    query_info.need_aggregate = query_analyzer->hasAggregation();

    if (storage && !options.only_analyze)
    {
        query_info.prepared_sets = query_analyzer->getPreparedSets();
        from_stage = storage->getQueryProcessingStage(context, options.to_stage, storage_snapshot, query_info);
    }

    /// Do I need to perform the first part of the pipeline?
    /// Running on remote servers during distributed processing or if query is not distributed.
    ///
    /// Also note that with distributed_group_by_no_merge=1 or when there is
    /// only one remote server, it is equal to local query in terms of query
    /// stages (or when due to optimize_distributed_group_by_sharding_key the query was processed up to Complete stage).
    bool first_stage = from_stage < QueryProcessingStage::WithMergeableState
        && options.to_stage >= QueryProcessingStage::WithMergeableState;
    /// Do I need to execute the second part of the pipeline?
    /// Running on the initiating server during distributed processing or if query is not distributed.
    ///
    /// Also note that with distributed_group_by_no_merge=2 (i.e. when optimize_distributed_group_by_sharding_key takes place)
    /// the query on the remote server will be processed up to WithMergeableStateAfterAggregationAndLimit,
    /// So it will do partial second stage (second_stage=true), and initiator will do the final part.
    bool second_stage = from_stage <= QueryProcessingStage::WithMergeableState
        && options.to_stage > QueryProcessingStage::WithMergeableState;

    analysis_result = ExpressionAnalysisResult(
        *query_analyzer, metadata_snapshot, first_stage, second_stage, options.only_analyze, filter_info, additional_filter_info, *source_header);

    if (options.to_stage == QueryProcessingStage::Enum::FetchColumns)
    {
        auto header = *source_header;

        if (analysis_result.prewhere_info)
        {
            header = analysis_result.prewhere_info->prewhere_actions.updateHeader(header);
            if (analysis_result.prewhere_info->remove_prewhere_column)
                header.erase(analysis_result.prewhere_info->prewhere_column_name);
        }
        return header;
    }

    if (options.to_stage == QueryProcessingStage::Enum::WithMergeableState)
    {
        if (!analysis_result.need_aggregate)
        {
            // What's the difference with selected_columns?
            // Here we calculate the header we want from remote server after it
            // executes query up to WithMergeableState. When there is an ORDER BY,
            // it is executed on remote server firstly, then we execute merge
            // sort on initiator. To execute ORDER BY, we need to calculate the
            // ORDER BY keys. These keys might be not present among the final
            // SELECT columns given by the `selected_column`. This is why we have
            // to use proper keys given by the result columns of the
            // `before_order_by` expression actions.
            // Another complication is window functions -- if we have them, they
            // are calculated on initiator, before ORDER BY columns. In this case,
            // the shard has to return columns required for window function
            // calculation and further steps, given by the `before_window`
            // expression actions.
            // As of 21.6 this is broken: the actions in `before_window` might
            // not contain everything required for the ORDER BY step, but this
            // is a responsibility of ExpressionAnalyzer and is not a problem
            // with this code. See
            // https://github.com/ClickHouse/ClickHouse/issues/19857 for details.
            if (analysis_result.before_window)
                return analysis_result.before_window->dag.getResultColumns();

            // NOTE: should not handle before_limit_by specially since
            // WithMergeableState does not process LIMIT BY

            return analysis_result.before_order_by->dag.getResultColumns();
        }

        Block header = analysis_result.before_aggregation->dag.getResultColumns();

        Block res;

        if (analysis_result.use_grouping_set_key)
            res.insert({ nullptr, std::make_shared<DataTypeUInt64>(), "__grouping_set" });

        if (context->getSettingsRef()[Setting::group_by_use_nulls] && analysis_result.use_grouping_set_key)
        {
            for (const auto & key : query_analyzer->aggregationKeys())
                res.insert({nullptr, makeNullableSafe(header.getByName(key.name).type), key.name});
        }
        else
        {
            for (const auto & key : query_analyzer->aggregationKeys())
                res.insert({nullptr, header.getByName(key.name).type, key.name});
        }

        for (const auto & aggregate : query_analyzer->aggregates())
        {
            size_t arguments_size = aggregate.argument_names.size();
            DataTypes argument_types(arguments_size);
            for (size_t j = 0; j < arguments_size; ++j)
                argument_types[j] = header.getByName(aggregate.argument_names[j]).type;

            DataTypePtr type = std::make_shared<DataTypeAggregateFunction>(aggregate.function, argument_types, aggregate.parameters);

            res.insert({nullptr, type, aggregate.column_name});
        }

        return res;
    }

    if (options.to_stage >= QueryProcessingStage::Enum::WithMergeableStateAfterAggregation)
    {
        // It's different from selected_columns, see the comment above for
        // WithMergeableState stage.
        if (analysis_result.before_window)
            return analysis_result.before_window->dag.getResultColumns();

        // In case of query on remote shards executed up to
        // WithMergeableStateAfterAggregation*, they can process LIMIT BY,
        // since the initiator will not apply LIMIT BY again.
        if (analysis_result.before_limit_by)
            return analysis_result.before_limit_by->dag.getResultColumns();

        return analysis_result.before_order_by->dag.getResultColumns();
    }

    return analysis_result.final_projection->dag.getResultColumns();
}


static std::pair<Field, DataTypePtr> getWithFillFieldValue(const ASTPtr & node, ContextPtr context)
{
    auto field_type = evaluateConstantExpression(node, context);

    if (!isColumnedAsNumber(field_type.second))
        throw Exception(ErrorCodes::INVALID_WITH_FILL_EXPRESSION,
                        "Illegal type {} of WITH FILL expression, must be numeric type", field_type.second->getName());

    return field_type;
}

static std::pair<Field, std::optional<IntervalKind>> getWithFillStep(const ASTPtr & node, const ContextPtr & context)
{
    auto [field, type] = evaluateConstantExpression(node, context);

    if (const auto * type_interval = typeid_cast<const DataTypeInterval *>(type.get()))
        return std::make_pair(std::move(field), type_interval->getKind());

    if (isColumnedAsNumber(type))
        return std::make_pair(std::move(field), std::nullopt);

    throw Exception(ErrorCodes::INVALID_WITH_FILL_EXPRESSION,
                    "Illegal type {} of WITH FILL expression, must be numeric type", type->getName());
}

static FillColumnDescription getWithFillDescription(const ASTOrderByElement & order_by_elem, const ContextPtr & context)
{
    FillColumnDescription descr;

    if (order_by_elem.getFillFrom())
        std::tie(descr.fill_from, descr.fill_from_type) = getWithFillFieldValue(order_by_elem.getFillFrom(), context);
    if (order_by_elem.getFillTo())
        std::tie(descr.fill_to, descr.fill_to_type) = getWithFillFieldValue(order_by_elem.getFillTo(), context);

    if (order_by_elem.getFillStep())
        std::tie(descr.fill_step, descr.step_kind) = getWithFillStep(order_by_elem.getFillStep(), context);
    else
        descr.fill_step = order_by_elem.direction;

    if (accurateEquals(descr.fill_step, Field{0}))
        throw Exception(ErrorCodes::INVALID_WITH_FILL_EXPRESSION, "WITH FILL STEP value cannot be zero");

    if (order_by_elem.direction == 1)
    {
        if (accurateLess(descr.fill_step, Field{0}))
            throw Exception(ErrorCodes::INVALID_WITH_FILL_EXPRESSION, "WITH FILL STEP value cannot be negative for sorting in ascending direction");

        if (!descr.fill_from.isNull() && !descr.fill_to.isNull() &&
            accurateLess(descr.fill_to, descr.fill_from))
        {
            throw Exception(ErrorCodes::INVALID_WITH_FILL_EXPRESSION,
                            "WITH FILL TO value cannot be less than FROM value for sorting in ascending direction");
        }
    }
    else
    {
        if (accurateLess(Field{0}, descr.fill_step))
            throw Exception(ErrorCodes::INVALID_WITH_FILL_EXPRESSION, "WITH FILL STEP value cannot be positive for sorting in descending direction");

        if (!descr.fill_from.isNull() && !descr.fill_to.isNull() &&
            accurateLess(descr.fill_from, descr.fill_to))
        {
            throw Exception(ErrorCodes::INVALID_WITH_FILL_EXPRESSION,
                            "WITH FILL FROM value cannot be less than TO value for sorting in descending direction");
        }
    }

    return descr;
}

SortDescription InterpreterSelectQuery::getSortDescription(const ASTSelectQuery & query, const ContextPtr & context_)
{
    SortDescription order_descr;
    order_descr.reserve(query.orderBy()->children.size());

    for (const auto & elem : query.orderBy()->children)
    {
        const String & column_name = elem->children.front()->getColumnName();
        const auto & order_by_elem = elem->as<ASTOrderByElement &>();

        std::shared_ptr<Collator> collator;
        if (order_by_elem.getCollation())
            collator = std::make_shared<Collator>(order_by_elem.getCollation()->as<ASTLiteral &>().value.safeGet<String>());

        if (order_by_elem.with_fill)
        {
            FillColumnDescription fill_desc = getWithFillDescription(order_by_elem, context_);
            order_descr.emplace_back(column_name, order_by_elem.direction, order_by_elem.nulls_direction, collator, true, fill_desc);
        }
        else
            order_descr.emplace_back(column_name, order_by_elem.direction, order_by_elem.nulls_direction, collator);
    }

    order_descr.compile_sort_description = context_->getSettingsRef()[Setting::compile_sort_description];
    order_descr.min_count_to_compile_sort_description = context_->getSettingsRef()[Setting::min_count_to_compile_sort_description];

    return order_descr;
}

static InterpolateDescriptionPtr getInterpolateDescription(
    const ASTSelectQuery & query, const Block & source_block, const Block & result_block, const Aliases & aliases, ContextPtr context)
{
    InterpolateDescriptionPtr interpolate_descr;
    if (query.interpolate())
    {
        NamesAndTypesList source_columns;
        ColumnsWithTypeAndName result_columns;
        ASTPtr exprs = std::make_shared<ASTExpressionList>();

        if (query.interpolate()->children.empty())
        {
            std::unordered_map<String, DataTypePtr> column_names;
            for (const auto & column : result_block.getColumnsWithTypeAndName())
                column_names[column.name] = column.type;
            for (const auto & elem : query.orderBy()->children)
                if (elem->as<ASTOrderByElement>()->with_fill)
                    column_names.erase(elem->as<ASTOrderByElement>()->children.front()->getColumnName());
            for (const auto & [name, type] : column_names)
            {
                source_columns.emplace_back(name, type);
                result_columns.emplace_back(type, name);
                exprs->children.emplace_back(std::make_shared<ASTIdentifier>(name));
            }
        }
        else
        {
            NameSet col_set;
            for (const auto & elem : query.interpolate()->children)
            {
                const auto & interpolate = elem->as<ASTInterpolateElement &>();

                if (const ColumnWithTypeAndName *result_block_column = result_block.findByName(interpolate.column))
                {
                    if (!col_set.insert(result_block_column->name).second)
                        throw Exception(ErrorCodes::INVALID_WITH_FILL_EXPRESSION,
                            "Duplicate INTERPOLATE column '{}'", interpolate.column);

                    result_columns.emplace_back(result_block_column->type, result_block_column->name);
                }
                else
                    throw Exception(ErrorCodes::UNKNOWN_IDENTIFIER,
                        "Missing column '{}' as an INTERPOLATE expression target", interpolate.column);

                exprs->children.emplace_back(interpolate.expr->clone());
            }

            col_set.clear();
            for (const auto & column : result_block)
            {
                source_columns.emplace_back(column.name, column.type);
                col_set.insert(column.name);
            }
            for (const auto & column : source_block)
                if (!col_set.contains(column.name))
                    source_columns.emplace_back(column.name, column.type);
        }

        auto syntax_result = TreeRewriter(context).analyze(exprs, source_columns);
        ExpressionAnalyzer analyzer(exprs, syntax_result, context);
        ActionsDAG actions = analyzer.getActionsDAG(true);
        ActionsDAG conv_dag = ActionsDAG::makeConvertingActions(actions.getResultColumns(),
            result_columns, ActionsDAG::MatchColumnsMode::Position, true);
        ActionsDAG merge_dag = ActionsDAG::merge(std::move(actions), std::move(conv_dag));

        interpolate_descr = std::make_shared<InterpolateDescription>(std::move(merge_dag), aliases);
    }

    return interpolate_descr;
}

static SortDescription getSortDescriptionFromGroupBy(const ASTSelectQuery & query)
{
    if (!query.groupBy())
        return {};

    SortDescription order_descr;
    order_descr.reserve(query.groupBy()->children.size());

    for (const auto & elem : query.groupBy()->children)
    {
        String name = elem->getColumnName();
        order_descr.emplace_back(name, 1, 1);
    }

    return order_descr;
}

static UInt64 getLimitUIntValue(const ASTPtr & node, const ContextPtr & context, const std::string & expr)
{
    const auto & [field, type] = evaluateConstantExpression(node, context);

    if (!isNativeNumber(type))
        throw Exception(ErrorCodes::INVALID_LIMIT_EXPRESSION, "Illegal type {} of {} expression, must be numeric type",
            type->getName(), expr);

    Field converted = convertFieldToType(field, DataTypeUInt64());
    if (converted.isNull())
        throw Exception(ErrorCodes::INVALID_LIMIT_EXPRESSION, "The value {} of {} expression is not representable as UInt64",
            applyVisitor(FieldVisitorToString(), field), expr);

    return converted.safeGet<UInt64>();
}


std::pair<UInt64, UInt64> InterpreterSelectQuery::getLimitLengthAndOffset(const ASTSelectQuery & query, const ContextPtr & context_)
{
    UInt64 length = 0;
    UInt64 offset = 0;

    if (query.limitLength())
    {
        length = getLimitUIntValue(query.limitLength(), context_, "LIMIT");
        if (query.limitOffset() && length)
            offset = getLimitUIntValue(query.limitOffset(), context_, "OFFSET");
    }
    else if (query.limitOffset())
        offset = getLimitUIntValue(query.limitOffset(), context_, "OFFSET");
    return {length, offset};
}


UInt64 InterpreterSelectQuery::getLimitForSorting(const ASTSelectQuery & query, const ContextPtr & context_)
{
    /// Partial sort can be done if there is LIMIT but no DISTINCT or LIMIT BY, neither ARRAY JOIN.
    if (!query.distinct && !query.limitBy() && !query.limit_with_ties && !query.arrayJoinExpressionList().first && query.limitLength())
    {
        auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context_);
        if (limit_length > std::numeric_limits<UInt64>::max() - limit_offset)
            return 0;

        return limit_length + limit_offset;
    }
    return 0;
}


static bool hasWithTotalsInAnySubqueryInFromClause(const ASTSelectQuery & query)
{
    if (query.group_by_with_totals)
        return true;

    /** NOTE You can also check that the table in the subquery is distributed, and that it only looks at one shard.
     * In other cases, totals will be computed on the initiating server of the query, and it is not necessary to read the data to the end.
     */
    if (auto query_table = extractTableExpression(query, 0))
    {
        if (const auto * ast_union = query_table->as<ASTSelectWithUnionQuery>())
        {
            /** NOTE
            * 1. For ASTSelectWithUnionQuery after normalization for union child node the height of the AST tree is at most 2.
            * 2. For ASTSelectIntersectExceptQuery after normalization in case there are intersect or except nodes,
            * the height of the AST tree can have any depth (each intersect/except adds a level), but the
            * number of children in those nodes is always 2.
            */
            std::function<bool(ASTPtr)> traverse_recursively = [&](ASTPtr child_ast) -> bool
            {
                if (const auto * select_child = child_ast->as <ASTSelectQuery>())
                {
                    if (hasWithTotalsInAnySubqueryInFromClause(select_child->as<ASTSelectQuery &>()))
                        return true;
                }
                else if (const auto * union_child = child_ast->as<ASTSelectWithUnionQuery>())
                {
                    for (const auto & subchild : union_child->list_of_selects->children)
                        if (traverse_recursively(subchild))
                            return true;
                }
                else if (const auto * intersect_child = child_ast->as<ASTSelectIntersectExceptQuery>())
                {
                    auto selects = intersect_child->getListOfSelects();
                    for (const auto & subchild : selects)
                        if (traverse_recursively(subchild))
                            return true;
                }
                return false;
            };

            for (const auto & elem : ast_union->list_of_selects->children)
                if (traverse_recursively(elem))
                    return true;
        }
    }

    return false;
}


void InterpreterSelectQuery::executeImpl(QueryPlan & query_plan, std::optional<Pipe> prepared_pipe)
{
    ProfileEvents::increment(ProfileEvents::SelectQueriesWithSubqueries);
    ProfileEvents::increment(ProfileEvents::QueriesWithSubqueries);

    /** Streams of data. When the query is executed in parallel, we have several data streams.
     *  If there is no GROUP BY, then perform all operations before ORDER BY and LIMIT in parallel, then
     *  if there is an ORDER BY, then glue the streams using ResizeProcessor, and then MergeSorting transforms,
     *  if not, then glue it using ResizeProcessor,
     *  then apply LIMIT.
     *  If there is GROUP BY, then we will perform all operations up to GROUP BY, inclusive, in parallel;
     *  a parallel GROUP BY will glue streams into one,
     *  then perform the remaining operations with one resulting stream.
     */

    /// Now we will compose block streams that perform the necessary actions.
    auto & query = getSelectQuery();
    const Settings & settings = context->getSettingsRef();
    auto & expressions = analysis_result;
    bool intermediate_stage = false;
    bool to_aggregation_stage = false;
    bool from_aggregation_stage = false;

    /// Do I need to aggregate in a separate row that has not passed max_rows_to_group_by?
    bool aggregate_overflow_row =
        expressions.need_aggregate &&
        query.group_by_with_totals &&
        settings[Setting::max_rows_to_group_by] &&
        settings[Setting::group_by_overflow_mode] == OverflowMode::ANY &&
        settings[Setting::totals_mode] != TotalsMode::AFTER_HAVING_EXCLUSIVE;

    /// Do I need to immediately finalize the aggregate functions after the aggregation?
    bool aggregate_final =
        expressions.need_aggregate &&
        options.to_stage > QueryProcessingStage::WithMergeableState &&
        !query.group_by_with_totals && !query.group_by_with_rollup && !query.group_by_with_cube;

    bool use_grouping_set_key = expressions.use_grouping_set_key;

    if (query.group_by_with_grouping_sets && query.group_by_with_totals)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "WITH TOTALS and GROUPING SETS are not supported together");

    if (query.group_by_with_grouping_sets && (query.group_by_with_rollup || query.group_by_with_cube))
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "GROUPING SETS are not supported together with ROLLUP and CUBE");

    if (expressions.hasHaving() && query.group_by_with_totals && (query.group_by_with_rollup || query.group_by_with_cube))
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "WITH TOTALS and WITH ROLLUP or CUBE are not supported together in presence of HAVING");

    if (query.qualify())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "QUALIFY clause is not supported in the old analyzer");

    if (options.only_analyze)
    {
        auto read_nothing = std::make_unique<ReadNothingStep>(source_header);
        query_plan.addStep(std::move(read_nothing));

        if (expressions.filter_info)
        {
            auto row_level_security_step = std::make_unique<FilterStep>(
                query_plan.getCurrentHeader(),
                expressions.filter_info->actions.clone(),
                expressions.filter_info->column_name,
                expressions.filter_info->do_remove_column);

            row_level_security_step->setStepDescription("Row-level security filter");
            query_plan.addStep(std::move(row_level_security_step));
        }

        if (expressions.prewhere_info)
        {
            if (expressions.prewhere_info->row_level_filter)
            {
                auto row_level_filter_step = std::make_unique<FilterStep>(
                    query_plan.getCurrentHeader(),
                    expressions.prewhere_info->row_level_filter->clone(),
                    expressions.prewhere_info->row_level_column_name,
                    true);

                row_level_filter_step->setStepDescription("Row-level security filter (PREWHERE)");
                query_plan.addStep(std::move(row_level_filter_step));
            }

            auto prewhere_step = std::make_unique<FilterStep>(
                query_plan.getCurrentHeader(),
                expressions.prewhere_info->prewhere_actions.clone(),
                expressions.prewhere_info->prewhere_column_name,
                expressions.prewhere_info->remove_prewhere_column);

            prewhere_step->setStepDescription("PREWHERE");
            query_plan.addStep(std::move(prewhere_step));
        }
    }
    else
    {
        if (prepared_pipe)
        {
            auto prepared_source_step = std::make_unique<ReadFromPreparedSource>(std::move(*prepared_pipe));
            query_plan.addStep(std::move(prepared_source_step));
            query_plan.addInterpreterContext(context);
        }

        if (from_stage == QueryProcessingStage::WithMergeableState &&
            options.to_stage == QueryProcessingStage::WithMergeableState)
            intermediate_stage = true;

        /// Support optimize_distributed_group_by_sharding_key
        /// Is running on the initiating server during distributed processing?
        if (from_stage >= QueryProcessingStage::WithMergeableStateAfterAggregation)
            from_aggregation_stage = true;
        /// Is running on remote servers during distributed processing?
        if (options.to_stage >= QueryProcessingStage::WithMergeableStateAfterAggregation)
            to_aggregation_stage = true;

        /// Read the data from Storage. from_stage - to what stage the request was completed in Storage.
        executeFetchColumns(from_stage, query_plan);

        LOG_TRACE(log, "{} -> {}", QueryProcessingStage::toString(from_stage), QueryProcessingStage::toString(options.to_stage));
    }

    InputOrderInfoPtr input_order_info_for_order;
    if (!expressions.need_aggregate)
        input_order_info_for_order = query_info.input_order_info;

    if (options.to_stage > QueryProcessingStage::FetchColumns)
    {
        auto preliminary_sort = [&]()
        {
            /** For distributed query processing,
              *  if no GROUP, HAVING set,
              *  but there is an ORDER or LIMIT,
              *  then we will perform the preliminary sorting and LIMIT on the remote server.
              */
            if (!expressions.second_stage
                && !expressions.need_aggregate
                && !expressions.hasHaving()
                && !expressions.has_window)
            {
                if (expressions.has_order_by)
                    executeOrder(query_plan, input_order_info_for_order);

                /// pre_distinct = false, because if we have limit and distinct,
                /// we need to merge streams to one and calculate overall distinct.
                /// Otherwise we can take several equal values from different streams
                /// according to limit and skip some distinct values.
                if (query.limitLength())
                    executeDistinct(query_plan, false, expressions.selected_columns, false);

                /// In case of query executed on remote shards (up to
                /// WithMergeableState*) LIMIT BY cannot be applied, since it
                /// will be applied on the initiator as well, and the header
                /// may not match in some obscure cases.
                if (options.to_stage == QueryProcessingStage::FetchColumns && expressions.hasLimitBy())
                {
                    executeExpression(query_plan, expressions.before_limit_by, "Before LIMIT BY");
                    executeLimitBy(query_plan);
                }

                /// WITH TIES simply not supported properly for preliminary steps, so let's disable it.
                if (query.limitLength() && !query.limitBy() && !query.limit_with_ties)
                    executePreLimit(query_plan, true);
            }
        };

        if (intermediate_stage)
        {
            if (expressions.first_stage || expressions.second_stage)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Query with intermediate stage cannot have any other stages");

            preliminary_sort();
            if (expressions.need_aggregate)
                executeMergeAggregated(query_plan, aggregate_overflow_row, aggregate_final, use_grouping_set_key);
        }

        if (from_aggregation_stage)
        {
            if (intermediate_stage || expressions.first_stage || expressions.second_stage)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Query with after aggregation stage cannot have any other stages");
        }

        if (expressions.first_stage)
        {
            // If there is a storage that supports prewhere, this will always be nullptr
            // Thus, we don't actually need to check if projection is active.
            if (expressions.filter_info)
            {
                auto row_level_security_step = std::make_unique<FilterStep>(
                    query_plan.getCurrentHeader(),
                    expressions.filter_info->actions.clone(),
                    expressions.filter_info->column_name,
                    expressions.filter_info->do_remove_column);

                row_level_security_step->setStepDescription("Row-level security filter");
                query_plan.addStep(std::move(row_level_security_step));
            }

            const auto add_filter_step = [&](auto & new_filter_info, const std::string & description)
            {
                auto filter_step = std::make_unique<FilterStep>(
                    query_plan.getCurrentHeader(),
                    std::move(new_filter_info->actions),
                    new_filter_info->column_name,
                    new_filter_info->do_remove_column);

                filter_step->setStepDescription(description);
                query_plan.addStep(std::move(filter_step));
            };

            if (additional_filter_info)
                add_filter_step(additional_filter_info, "Additional filter");

            if (parallel_replicas_custom_filter_info)
                add_filter_step(parallel_replicas_custom_filter_info, "Parallel replica custom key filter");

            if (expressions.before_array_join)
                executeExpression(query_plan, expressions.before_array_join, "Before ARRAY JOIN");

            if (expressions.array_join)
            {
                QueryPlanStepPtr array_join_step
                    = std::make_unique<ArrayJoinStep>(
                        query_plan.getCurrentHeader(),
                        *expressions.array_join,
                        settings[Setting::enable_unaligned_array_join],
                        settings[Setting::max_block_size]);

                array_join_step->setStepDescription("ARRAY JOIN");
                query_plan.addStep(std::move(array_join_step));
            }

            if (expressions.before_join)
                executeExpression(query_plan, expressions.before_join, "Before JOIN");

            /// Optional step to convert key columns to common supertype.
            if (expressions.converting_join_columns)
                executeExpression(query_plan, expressions.converting_join_columns, "Convert JOIN columns");

            if (expressions.hasJoin())
            {
                if (expressions.join->isFilled())
                {
                    QueryPlanStepPtr filled_join_step
                        = std::make_unique<FilledJoinStep>(query_plan.getCurrentHeader(), expressions.join, settings[Setting::max_block_size]);

                    filled_join_step->setStepDescription("JOIN");
                    query_plan.addStep(std::move(filled_join_step));
                }
                else
                {
                    auto joined_plan = query_analyzer->getJoinedPlan();

                    if (!joined_plan)
                        throw Exception(ErrorCodes::LOGICAL_ERROR, "There is no joined plan for query");

                    auto add_sorting = [this] (QueryPlan & plan, const Names & key_names, JoinTableSide join_pos)
                    {
                        SortDescription order_descr;
                        order_descr.reserve(key_names.size());
                        for (const auto & key_name : key_names)
                            order_descr.emplace_back(key_name);

                        SortingStep::Settings sort_settings(context->getSettingsRef());

                        auto sorting_step = std::make_unique<SortingStep>(
                            plan.getCurrentHeader(),
                            std::move(order_descr),
                            0 /* LIMIT */, sort_settings);
                        sorting_step->setStepDescription(fmt::format("Sort {} before JOIN", join_pos));
                        plan.addStep(std::move(sorting_step));
                    };

                    auto crosswise_connection = CreateSetAndFilterOnTheFlyStep::createCrossConnection();
                    auto add_create_set
                        = [&settings, crosswise_connection](QueryPlan & plan, const Names & key_names, JoinTableSide join_pos)
                    {
                        auto creating_set_step = std::make_unique<CreateSetAndFilterOnTheFlyStep>(
                            plan.getCurrentHeader(),
                            key_names,
                            settings[Setting::max_rows_in_set_to_optimize_join],
                            crosswise_connection,
                            join_pos);
                        creating_set_step->setStepDescription(fmt::format("Create set and filter {} joined stream", join_pos));

                        auto * step_raw_ptr = creating_set_step.get();
                        plan.addStep(std::move(creating_set_step));
                        return step_raw_ptr;
                    };

                    if (expressions.join->pipelineType() == JoinPipelineType::YShaped && expressions.join->getTableJoin().kind() != JoinKind::Paste)
                    {
                        const auto & table_join = expressions.join->getTableJoin();
                        const auto & join_clause = table_join.getOnlyClause();

                        auto join_kind = table_join.kind();
                        auto join_strictness = table_join.strictness();

                        bool join_type_allows_filtering = (join_strictness == JoinStrictness::All || join_strictness == JoinStrictness::Any)
                                                       && (isInner(join_kind) || isLeft(join_kind) || isRight(join_kind));

                        auto has_non_const = [](const Block & block, const auto & keys)
                        {
                            for (const auto & key : keys)
                            {
                                const auto & column = block.getByName(key).column;
                                if (column && !isColumnConst(*column))
                                    return true;
                            }
                            return false;
                        };
                        /// This optimization relies on the sorting that should buffer the whole stream before emitting any rows.
                        /// It doesn't hold such a guarantee for streams with const keys.
                        /// Note: it's also doesn't work with the read-in-order optimization.
                        /// No checks here because read in order is not applied if we have `CreateSetAndFilterOnTheFlyStep` in the pipeline between the reading and sorting steps.
                        bool has_non_const_keys = has_non_const(*query_plan.getCurrentHeader(), join_clause.key_names_left)
                            && has_non_const(*joined_plan->getCurrentHeader(), join_clause.key_names_right);

                        if (settings[Setting::max_rows_in_set_to_optimize_join] > 0 && join_type_allows_filtering && has_non_const_keys)
                        {
                            auto * left_set = add_create_set(query_plan, join_clause.key_names_left, JoinTableSide::Left);
                            auto * right_set = add_create_set(*joined_plan, join_clause.key_names_right, JoinTableSide::Right);

                            if (isInnerOrLeft(join_kind))
                                right_set->setFiltering(left_set->getSet());

                            if (isInnerOrRight(join_kind))
                                left_set->setFiltering(right_set->getSet());
                        }

                        add_sorting(query_plan, join_clause.key_names_left, JoinTableSide::Left);
                        add_sorting(*joined_plan, join_clause.key_names_right, JoinTableSide::Right);
                    }

                    QueryPlanStepPtr join_step = std::make_unique<JoinStep>(
                        query_plan.getCurrentHeader(),
                        joined_plan->getCurrentHeader(),
                        expressions.join,
                        settings[Setting::max_block_size],
                        /* min_block_size_rows_ */ 0,
                        /* min_block_size_bytes_ */ 0,
                        max_streams,
                        /* required_output_ = */ NameSet{},
                        analysis_result.optimize_read_in_order,
                        /* use_new_analyzer_ = */ false);

                    join_step->setStepDescription(fmt::format("JOIN {}", expressions.join->pipelineType()));
                    std::vector<QueryPlanPtr> plans;
                    plans.emplace_back(std::make_unique<QueryPlan>(std::move(query_plan)));
                    plans.emplace_back(std::move(joined_plan));

                    query_plan = QueryPlan();
                    query_plan.unitePlans(std::move(join_step), {std::move(plans)});
                }
            }

            if (expressions.hasWhere())
                executeWhere(query_plan, expressions.before_where, expressions.remove_where_filter);

            if (expressions.need_aggregate)
                executeAggregation(
                    query_plan, expressions.before_aggregation, aggregate_overflow_row, aggregate_final, query_info.input_order_info);

            // Now we must execute:
            // 1) expressions before window functions,
            // 2) window functions,
            // 3) expressions after window functions,
            // 4) preliminary distinct.
            // This code decides which part we execute on shard (first_stage)
            // and which part on initiator (second_stage). See also the counterpart
            // code for "second_stage" that has to execute the rest.
            if (expressions.need_aggregate)
            {
                // We have aggregation, so we can't execute any later-stage
                // expressions on shards, neither "before window functions" nor
                // "before ORDER BY".
            }
            else
            {
                // We don't have aggregation.
                // Window functions must be executed on initiator (second_stage).
                // ORDER BY and DISTINCT might depend on them, so if we have
                // window functions, we can't execute ORDER BY and DISTINCT
                // now, on shard (first_stage).
                if (query_analyzer->hasWindow())
                {
                    executeExpression(query_plan, expressions.before_window, "Before window functions");
                }
                else
                {
                    // We don't have window functions, so we can execute the
                    // expressions before ORDER BY and the preliminary DISTINCT
                    // now, on shards (first_stage).
                    assert(!expressions.before_window);
                    executeExpression(query_plan, expressions.before_order_by, "Before ORDER BY");
                    executeDistinct(query_plan, true, expressions.selected_columns, true);
                }
            }

            preliminary_sort();
        }

        if (expressions.second_stage || from_aggregation_stage)
        {
            if (from_aggregation_stage)
            {
                /// No need to aggregate anything, since this was done on remote shards.
            }
            else if (expressions.need_aggregate)
            {
                /// If you need to combine aggregated results from multiple servers
                if (!expressions.first_stage)
                    executeMergeAggregated(query_plan, aggregate_overflow_row, aggregate_final, use_grouping_set_key);

                if (!aggregate_final)
                {
                    if (query.group_by_with_totals)
                    {
                        bool final = !query.group_by_with_rollup && !query.group_by_with_cube;
                        executeTotalsAndHaving(
                            query_plan, expressions.hasHaving(), expressions.before_having, expressions.remove_having_filter, aggregate_overflow_row, final);
                    }

                    if (query.group_by_with_rollup)
                        executeRollupOrCube(query_plan, Modificator::ROLLUP);
                    else if (query.group_by_with_cube)
                        executeRollupOrCube(query_plan, Modificator::CUBE);

                    if ((query.group_by_with_rollup || query.group_by_with_cube || query.group_by_with_grouping_sets) && expressions.hasHaving())
                        executeHaving(query_plan, expressions.before_having, expressions.remove_having_filter);
                }
                else if (expressions.hasHaving())
                    executeHaving(query_plan, expressions.before_having, expressions.remove_having_filter);
            }
            else if (query.group_by_with_totals || query.group_by_with_rollup || query.group_by_with_cube || query.group_by_with_grouping_sets)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED, "WITH TOTALS, ROLLUP, CUBE or GROUPING SETS are not supported without aggregation");

            // Now we must execute:
            // 1) expressions before window functions,
            // 2) window functions,
            // 3) expressions after window functions,
            // 4) preliminary distinct.
            // Some of these were already executed at the shards (first_stage),
            // see the counterpart code and comments there.
            if (from_aggregation_stage)
            {
                if (query_analyzer->hasWindow())
                    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Window functions does not support processing from WithMergeableStateAfterAggregation");
            }
            else if (expressions.need_aggregate)
            {
                executeExpression(query_plan, expressions.before_window,
                    "Before window functions");
                executeWindow(query_plan);
                executeExpression(query_plan, expressions.before_order_by, "Before ORDER BY");
                executeDistinct(query_plan, true, expressions.selected_columns, true);
            }
            else
            {
                if (query_analyzer->hasWindow())
                {
                    executeWindow(query_plan);
                    executeExpression(query_plan, expressions.before_order_by, "Before ORDER BY");
                    executeDistinct(query_plan, true, expressions.selected_columns, true);
                }
                else
                {
                    // Neither aggregation nor windows, all expressions before
                    // ORDER BY executed on shards.
                }
            }

            if (expressions.has_order_by)
            {
                /** If there is an ORDER BY for distributed query processing,
                  *  but there is no aggregation, then on the remote servers ORDER BY was made
                  *  - therefore, we merge the sorted streams from remote servers.
                  *
                  * Also in case of remote servers was process the query up to WithMergeableStateAfterAggregationAndLimit
                  * (distributed_group_by_no_merge=2 or optimize_distributed_group_by_sharding_key=1 takes place),
                  * then merge the sorted streams is enough, since remote servers already did full ORDER BY.
                  */

                if (from_aggregation_stage)
                    executeMergeSorted(query_plan, "after aggregation stage for ORDER BY");
                else if (!expressions.first_stage
                    && !expressions.need_aggregate
                    && !expressions.has_window
                    && !(query.group_by_with_totals && !aggregate_final))
                    executeMergeSorted(query_plan, "for ORDER BY, without aggregation");
                else    /// Otherwise, just sort.
                    executeOrder(query_plan, input_order_info_for_order);
            }

            /** Optimization - if there are several sources and there is LIMIT, then first apply the preliminary LIMIT,
              * limiting the number of rows in each up to `offset + limit`.
              */
            bool has_withfill = false;
            if (query.orderBy())
            {
                SortDescription order_descr = getSortDescription(query, context);
                for (auto & desc : order_descr)
                    if (desc.with_fill)
                    {
                        has_withfill = true;
                        break;
                    }
            }

            bool apply_limit = options.to_stage != QueryProcessingStage::WithMergeableStateAfterAggregation;
            bool apply_prelimit = apply_limit &&
                                  query.limitLength() && !query.limit_with_ties &&
                                  !hasWithTotalsInAnySubqueryInFromClause(query) &&
                                  !query.arrayJoinExpressionList().first &&
                                  !query.distinct &&
                                  !expressions.hasLimitBy() &&
                                  !settings[Setting::extremes] &&
                                  !has_withfill;
            bool apply_offset = options.to_stage != QueryProcessingStage::WithMergeableStateAfterAggregationAndLimit;
            if (apply_prelimit)
            {
                executePreLimit(query_plan, /* do_not_skip_offset= */!apply_offset);
            }

            /** If there was more than one stream,
              * then DISTINCT needs to be performed once again after merging all streams.
              */
            if (!from_aggregation_stage && query.distinct)
                executeDistinct(query_plan, false, expressions.selected_columns, false);

            if (!from_aggregation_stage && expressions.hasLimitBy())
            {
                executeExpression(query_plan, expressions.before_limit_by, "Before LIMIT BY");
                executeLimitBy(query_plan);
            }

            executeWithFill(query_plan);

            /// If we have 'WITH TIES', we need execute limit before projection,
            /// because in that case columns from 'ORDER BY' are used.
            if (query.limit_with_ties && apply_limit && apply_offset)
            {
                executeLimit(query_plan);
            }

            /// Projection not be done on the shards, since then initiator will not find column in blocks.
            /// (significant only for WithMergeableStateAfterAggregation/WithMergeableStateAfterAggregationAndLimit).
            if (!to_aggregation_stage)
            {
                /// We must do projection after DISTINCT because projection may remove some columns.
                executeProjection(query_plan, expressions.final_projection);
            }

            /// Extremes are calculated before LIMIT, but after LIMIT BY. This is Ok.
            executeExtremes(query_plan);

            bool limit_applied = apply_prelimit || (query.limit_with_ties && apply_offset);
            /// Limit is no longer needed if there is prelimit.
            ///
            /// NOTE: that LIMIT cannot be applied if OFFSET should not be applied,
            /// since LIMIT will apply OFFSET too.
            /// This is the case for various optimizations for distributed queries,
            /// and when LIMIT cannot be applied it will be applied on the initiator anyway.
            if (apply_limit && !limit_applied && apply_offset)
                executeLimit(query_plan);

            if (apply_offset)
                executeOffset(query_plan);
        }
    }

    executeSubqueriesInSetsAndJoins(query_plan);
}

static void executeMergeAggregatedImpl(
    QueryPlan & query_plan,
    bool overflow_row,
    bool final,
    bool is_remote_storage,
    bool has_grouping_sets,
    const Settings & settings,
    const NamesAndTypesList & aggregation_keys,
    const NamesAndTypesLists & aggregation_keys_list,
    const AggregateDescriptions & aggregates,
    bool should_produce_results_in_order_of_bucket_number)
{
    auto keys = aggregation_keys.getNames();

    /** There are two modes of distributed aggregation.
      *
      * 1. In different threads read from the remote servers blocks.
      * Save all the blocks in the RAM. Merge blocks.
      * If the aggregation is two-level - parallelize to the number of buckets.
      *
      * 2. In one thread, read blocks from different servers in order.
      * RAM stores only one block from each server.
      * If the aggregation is a two-level aggregation, we consistently merge the blocks of each next level.
      *
      * The second option consumes less memory (up to 256 times less)
      *  in the case of two-level aggregation, which is used for large results after GROUP BY,
      *  but it can work more slowly.
      */

    Aggregator::Params params(
        keys,
        aggregates,
        overflow_row,
        settings[Setting::max_threads],
        settings[Setting::max_block_size],
        settings[Setting::min_hit_rate_to_use_consecutive_keys_optimization]);
    auto grouping_sets_params = getAggregatorGroupingSetsParams(aggregation_keys_list, keys);

    auto merging_aggregated = std::make_unique<MergingAggregatedStep>(
        query_plan.getCurrentHeader(),
        params,
        grouping_sets_params,
        final,
        /// Grouping sets don't work with distributed_aggregation_memory_efficient enabled (#43989)
        settings[Setting::distributed_aggregation_memory_efficient] && is_remote_storage && !has_grouping_sets,
        settings[Setting::aggregation_memory_efficient_merge_threads],
        should_produce_results_in_order_of_bucket_number,
        settings[Setting::max_block_size],
        settings[Setting::aggregation_in_order_max_block_bytes],
        settings[Setting::enable_memory_bound_merging_of_aggregation_results]);

    query_plan.addStep(std::move(merging_aggregated));
}

void InterpreterSelectQuery::addEmptySourceToQueryPlan(QueryPlan & query_plan, const Block & source_header, const SelectQueryInfo & query_info)
{
    Pipe pipe(std::make_shared<NullSource>(std::make_shared<const Block>(source_header)));

    if (query_info.prewhere_info)
    {
        auto & prewhere_info = *query_info.prewhere_info;

        if (prewhere_info.row_level_filter)
        {
            auto row_level_actions = std::make_shared<ExpressionActions>(prewhere_info.row_level_filter->clone());
            pipe.addSimpleTransform([&](const SharedHeader & header)
            {
                return std::make_shared<FilterTransform>(header,
                    row_level_actions,
                    prewhere_info.row_level_column_name, true);
            });
        }

        auto filter_actions = std::make_shared<ExpressionActions>(prewhere_info.prewhere_actions.clone());
        pipe.addSimpleTransform([&](const SharedHeader & header)
        {
            return std::make_shared<FilterTransform>(
                header, filter_actions,
                prewhere_info.prewhere_column_name, prewhere_info.remove_prewhere_column);
        });
    }

    auto read_from_pipe = std::make_unique<ReadFromPreparedSource>(std::move(pipe));
    read_from_pipe->setStepDescription("Read from NullSource");
    query_plan.addStep(std::move(read_from_pipe));
}

RowPolicyFilterPtr InterpreterSelectQuery::getRowPolicyFilter() const
{
    return row_policy_filter;
}

void InterpreterSelectQuery::extendQueryLogElemImpl(QueryLogElement & elem, const ASTPtr & /*ast*/, ContextPtr /*context_*/) const
{
    if (row_policy_filter)
    {
        for (const auto & row_policy : row_policy_filter->policies)
        {
            auto name = row_policy->getFullName().toString();
            elem.used_row_policies.emplace(std::move(name));
        }
    }
}

bool InterpreterSelectQuery::shouldMoveToPrewhere() const
{
    const Settings & settings = context->getSettingsRef();
    const ASTSelectQuery & query = query_ptr->as<const ASTSelectQuery &>();
    return settings[Setting::optimize_move_to_prewhere] && (!query.final() || settings[Setting::optimize_move_to_prewhere_if_final]);
}

/// Note that this is const and accepts the analysis ref to be able to use it to do analysis for parallel replicas
/// without affecting the final analysis multiple times
void InterpreterSelectQuery::applyFiltersToPrewhereInAnalysis(ExpressionAnalysisResult & analysis) const
{
    if (!analysis.filter_info)
        return;

    if (!analysis.prewhere_info)
    {
        const bool does_storage_support_prewhere = !input_pipe && storage && storage->supportsPrewhere();
        if (does_storage_support_prewhere && shouldMoveToPrewhere())
        {
            /// Execute row level filter in prewhere as a part of "move to prewhere" optimization.
            analysis.prewhere_info = std::make_shared<PrewhereInfo>(std::move(analysis.filter_info->actions), analysis.filter_info->column_name);
            analysis.prewhere_info->remove_prewhere_column = std::move(analysis.filter_info->do_remove_column);
            analysis.prewhere_info->need_filter = true;
            analysis.filter_info = nullptr;
        }
    }
    else
    {
        /// Add row level security actions to prewhere.
        analysis.prewhere_info->row_level_filter = std::move(analysis.filter_info->actions);
        analysis.prewhere_info->row_level_column_name = std::move(analysis.filter_info->column_name);
        analysis.filter_info = nullptr;
    }
}


void InterpreterSelectQuery::addPrewhereAliasActions()
{
    applyFiltersToPrewhereInAnalysis(analysis_result);
    auto & prewhere_info = analysis_result.prewhere_info;
    auto & columns_to_remove_after_prewhere = analysis_result.columns_to_remove_after_prewhere;

    /// Detect, if ALIAS columns are required for query execution
    auto alias_columns_required = false;
    const ColumnsDescription & storage_columns = metadata_snapshot->getColumns();
    for (const auto & column_name : required_columns)
    {
        auto column_default = storage_columns.getDefault(column_name);
        if (column_default && column_default->kind == ColumnDefaultKind::Alias)
        {
            alias_columns_required = true;
            break;
        }
    }

    /// Set of all (including ALIAS) required columns for PREWHERE
    auto get_prewhere_columns = [&]()
    {
        NameSet columns;

        if (prewhere_info)
        {
            /// Get some columns directly from PREWHERE expression actions
            auto prewhere_required_columns = prewhere_info->prewhere_actions.getRequiredColumns().getNames();
            columns.insert(prewhere_required_columns.begin(), prewhere_required_columns.end());

            if (prewhere_info->row_level_filter)
            {
                auto row_level_required_columns = prewhere_info->row_level_filter->getRequiredColumns().getNames();
                columns.insert(row_level_required_columns.begin(), row_level_required_columns.end());
            }
        }

        return columns;
    };

    /// There are multiple sources of required columns:
    ///  - raw required columns,
    ///  - columns deduced from ALIAS columns,
    ///  - raw required columns from PREWHERE,
    ///  - columns deduced from ALIAS columns from PREWHERE.
    /// PREWHERE is a special case, since we need to resolve it and pass directly to `IStorage::read()`
    /// before any other executions.
    if (alias_columns_required)
    {
        NameSet required_columns_from_prewhere = get_prewhere_columns();
        NameSet required_aliases_from_prewhere; /// Set of ALIAS required columns for PREWHERE

        /// Expression, that contains all raw required columns
        ASTPtr required_columns_all_expr = std::make_shared<ASTExpressionList>();

        /// Expression, that contains raw required columns for PREWHERE
        ASTPtr required_columns_from_prewhere_expr = std::make_shared<ASTExpressionList>();

        /// Sort out already known required columns between expressions,
        /// also populate `required_aliases_from_prewhere`.
        for (const auto & column : required_columns)
        {
            ASTPtr column_expr;
            const auto column_default = storage_columns.getDefault(column);
            bool is_alias = column_default && column_default->kind == ColumnDefaultKind::Alias;
            if (is_alias)
            {
                auto column_decl = storage_columns.get(column);
                column_expr = column_default->expression->clone();
                // recursive visit for alias to alias
                replaceAliasColumnsInQuery(
                    column_expr, metadata_snapshot->getColumns(), syntax_analyzer_result->array_join_result_to_source, context);

                column_expr = addTypeConversionToAST(
                    std::move(column_expr), column_decl.type->getName(), metadata_snapshot->getColumns().getAll(), context);
                column_expr = setAlias(column_expr, column);
            }
            else
                column_expr = std::make_shared<ASTIdentifier>(column);

            if (required_columns_from_prewhere.contains(column))
            {
                required_columns_from_prewhere_expr->children.emplace_back(std::move(column_expr));

                if (is_alias)
                    required_aliases_from_prewhere.insert(column);
            }
            else
                required_columns_all_expr->children.emplace_back(std::move(column_expr));
        }

        /// Columns, which we will get after prewhere and filter executions.
        NamesAndTypesList required_columns_after_prewhere;
        NameSet required_columns_after_prewhere_set;

        /// Collect required columns from prewhere expression actions.
        if (prewhere_info)
        {
            NameSet columns_to_remove(columns_to_remove_after_prewhere.begin(), columns_to_remove_after_prewhere.end());
            Block prewhere_actions_result = prewhere_info->prewhere_actions.getResultColumns();

            /// Populate required columns with the columns, added by PREWHERE actions and not removed afterwards.
            /// XXX: looks hacky that we already know which columns after PREWHERE we won't need for sure.
            for (const auto & column : prewhere_actions_result)
            {
                if (prewhere_info->remove_prewhere_column && column.name == prewhere_info->prewhere_column_name)
                    continue;

                if (columns_to_remove.contains(column.name))
                    continue;

                required_columns_all_expr->children.emplace_back(std::make_shared<ASTIdentifier>(column.name));
                required_columns_after_prewhere.emplace_back(column.name, column.type);
            }

            required_columns_after_prewhere_set = NameSet{
                std::from_range_t{}, required_columns_after_prewhere | std::views::transform([](const auto & it) { return it.name; })};
        }

        auto syntax_result
            = TreeRewriter(context).analyze(required_columns_all_expr, required_columns_after_prewhere, storage, storage_snapshot, options.is_create_parameterized_view);
        alias_actions = ExpressionAnalyzer(required_columns_all_expr, syntax_result, context).getActionsDAG(true);

        /// The set of required columns could be added as a result of adding an action to calculate ALIAS.
        required_columns = alias_actions->getRequiredColumns().getNames();

        /// Do not remove prewhere filter if it is a column which is used as alias.
        if (prewhere_info && prewhere_info->remove_prewhere_column)
            if (required_columns.end() != std::find(required_columns.begin(), required_columns.end(), prewhere_info->prewhere_column_name))
                prewhere_info->remove_prewhere_column = false;

        /// Remove columns which will be added by prewhere.
        std::erase_if(required_columns, [&](const String & name) { return required_columns_after_prewhere_set.contains(name); });

        if (prewhere_info)
        {
            /// Don't remove columns which are needed to be aliased.
            for (const auto & name : required_columns)
                prewhere_info->prewhere_actions.tryRestoreColumn(name);

            /// Add physical columns required by prewhere actions.
            for (const auto & column : required_columns_from_prewhere)
                if (!required_aliases_from_prewhere.contains(column))
                    if (required_columns.end() == std::find(required_columns.begin(), required_columns.end(), column))
                        required_columns.push_back(column);
        }
    }

    const auto & supported_prewhere_columns = storage->supportedPrewhereColumns();
    if (supported_prewhere_columns.has_value())
    {
        NameSet required_columns_from_prewhere = get_prewhere_columns();

        for (const auto & column_name : required_columns_from_prewhere)
        {
            if (!supported_prewhere_columns->contains(column_name))
                throw Exception(ErrorCodes::ILLEGAL_PREWHERE, "Storage {} doesn't support PREWHERE for {}", storage->getName(), column_name);
        }
    }
}

/// Based on the query analysis, check if using a trivial count (storage or partition metadata) is possible
std::optional<UInt64> InterpreterSelectQuery::getTrivialCount(UInt64 allow_experimental_parallel_reading_from_replicas)
{
    const Settings & settings = context->getSettingsRef();
    bool optimize_trivial_count =
        syntax_analyzer_result->optimize_trivial_count
        && (allow_experimental_parallel_reading_from_replicas == 0)
        && !settings[Setting::allow_experimental_query_deduplication]
        && !settings[Setting::empty_result_for_aggregation_by_empty_set]
        && storage
        && storage->supportsTrivialCountOptimization(storage_snapshot, getContext())
        && query_info.filter_asts.empty()
        && query_analyzer->hasAggregation()
        && (query_analyzer->aggregates().size() == 1)
        && typeid_cast<const AggregateFunctionCount *>(query_analyzer->aggregates()[0].function.get());

    if (!optimize_trivial_count)
        return {};

    auto & query = getSelectQuery();
    if (!query.prewhere() && !query.where() && !context->getCurrentTransaction())
    {
        /// Some storages can optimize trivial count in read() method instead of totalRows() because it still can
        /// require reading some data (but much faster than reading columns).
        /// Set a special flag in query info so the storage will see it and optimize count in read() method.
        query_info.optimize_trivial_count = optimize_trivial_count;
        return storage->totalRows(context);
    }

    // It's possible to optimize count() given only partition predicates
    ActionsDAG::NodeRawConstPtrs filter_nodes;
    if (analysis_result.hasPrewhere())
    {
        auto & prewhere_info = analysis_result.prewhere_info;
        filter_nodes.push_back(&prewhere_info->prewhere_actions.findInOutputs(prewhere_info->prewhere_column_name));

        if (prewhere_info->row_level_filter)
            filter_nodes.push_back(&prewhere_info->row_level_filter->findInOutputs(prewhere_info->row_level_column_name));
    }
    if (analysis_result.hasWhere())
    {
        filter_nodes.push_back(&analysis_result.before_where->dag.findInOutputs(analysis_result.where_column_name));
    }

    auto filter_actions_dag = ActionsDAG::buildFilterActionsDAG(filter_nodes);
    if (!filter_actions_dag)
        return {};

    return storage->totalRowsByPartitionPredicate(*filter_actions_dag, context);
}

/** Optimization - if not specified DISTINCT, WHERE, GROUP, HAVING, ORDER, JOIN, LIMIT BY, WITH TIES
 *  but LIMIT is specified, and limit + offset < max_block_size,
 *  then as the block size we will use limit + offset (not to read more from the table than requested),
 *  and also set the number of threads to 1.
 */
UInt64 InterpreterSelectQuery::maxBlockSizeByLimit() const
{
    const auto & query = query_ptr->as<const ASTSelectQuery &>();

    auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);

    if (!query.distinct
       && !query.limit_with_ties
       && !query.prewhere()
       && !query.where()
       && query_info.filter_asts.empty()
       && !query.groupBy()
       && !query.having()
       && !query.orderBy()
       && !query.limitBy()
       && !query.join()
       && !query_analyzer->hasAggregation()
       && !query_analyzer->hasWindow()
       && query.limitLength()
       && limit_length <= std::numeric_limits<UInt64>::max() - limit_offset)
        return limit_length + limit_offset;

    return 0;
}

void InterpreterSelectQuery::executeFetchColumns(QueryProcessingStage::Enum processing_stage, QueryPlan & query_plan)
{
    auto & query = getSelectQuery();
    const Settings & settings = context->getSettingsRef();
    std::optional<UInt64> num_rows;

    /// Optimization for trivial query like SELECT count() FROM table.
    if (processing_stage == QueryProcessingStage::FetchColumns && (num_rows = getTrivialCount(settings[Setting::allow_experimental_parallel_reading_from_replicas])))
    {
        const auto & desc = query_analyzer->aggregates()[0];
        const auto & func = desc.function;
        const AggregateFunctionCount & agg_count = static_cast<const AggregateFunctionCount &>(*func);

        /// We will process it up to "WithMergeableState".
        std::vector<char> state(agg_count.sizeOfData());
        AggregateDataPtr place = state.data();

        agg_count.create(place);
        SCOPE_EXIT_MEMORY_SAFE(agg_count.destroy(place));

        AggregateFunctionCount::set(place, *num_rows);

        auto column = ColumnAggregateFunction::create(func);
        column->insertFrom(place);

        Block header = analysis_result.before_aggregation->dag.getResultColumns();
        size_t arguments_size = desc.argument_names.size();
        DataTypes argument_types(arguments_size);
        for (size_t j = 0; j < arguments_size; ++j)
            argument_types[j] = header.getByName(desc.argument_names[j]).type;

        auto block_with_count = std::make_shared<const Block>(Block{
            {std::move(column), std::make_shared<DataTypeAggregateFunction>(func, argument_types, desc.parameters), desc.column_name}});

        auto source = std::make_shared<SourceFromSingleChunk>(block_with_count);
        auto prepared_count = std::make_unique<ReadFromPreparedSource>(Pipe(std::move(source)));
        prepared_count->setStepDescription("Optimized trivial count");
        query_plan.addStep(std::move(prepared_count));
        from_stage = QueryProcessingStage::WithMergeableState;
        analysis_result.first_stage = false;
        return;
    }

    /// Limitation on the number of columns to read.
    /// It's not applied in 'only_analyze' mode, because the query could be analyzed without removal of unnecessary columns.
    if (!options.only_analyze && settings[Setting::max_columns_to_read] && required_columns.size() > settings[Setting::max_columns_to_read])
        throw Exception(
            ErrorCodes::TOO_MANY_COLUMNS,
            "Limit for number of columns to read exceeded. Requested: {}, maximum: {}",
            required_columns.size(),
            settings[Setting::max_columns_to_read].value);

    /// General limit for the number of threads.
    size_t max_threads_execute_query = settings[Setting::max_threads];

    /**
     * To simultaneously query more remote servers when async_socket_for_remote is off
     * instead of max_threads, max_distributed_connections is used:
     * since threads there mostly spend time waiting for data from remote servers,
     * we can increase the degree of parallelism to avoid sequential querying of remote servers.
     *
     * DANGER: that can lead to insane number of threads working if there are a lot of stream and prefer_localhost_replica is used.
     *
     * That is not needed when async_socket_for_remote is on, because in that case
     * threads are not blocked waiting for data from remote servers.
     *
     */
    bool is_sync_remote = false;
    if (storage && storage->isRemote() && !settings[Setting::async_socket_for_remote])
    {
        is_sync_remote = true;
        max_threads_execute_query = max_streams = settings[Setting::max_distributed_connections];
    }

    UInt64 max_block_size = settings[Setting::max_block_size];
    auto local_limits = getStorageLimits(*context, options);

    if (UInt64 max_block_limited = maxBlockSizeByLimit())
    {
        if (max_block_limited < max_block_size)
        {
            max_block_size = std::max<UInt64>(1, max_block_limited);
            max_threads_execute_query = max_streams = 1;
        }

        if (local_limits.local_limits.size_limits.max_rows != 0)
        {
            if (max_block_limited < local_limits.local_limits.size_limits.max_rows)
                query_info.trivial_limit = max_block_limited;
            else if (local_limits.local_limits.size_limits.max_rows < std::numeric_limits<UInt64>::max()) /// Ask to read just enough rows to make the max_rows limit effective (so it has a chance to be triggered).
                query_info.trivial_limit = 1 + local_limits.local_limits.size_limits.max_rows;
        }
        else
        {
            query_info.trivial_limit = max_block_limited;
        }
    }

    if (!max_block_size)
        throw Exception(ErrorCodes::PARAMETER_OUT_OF_BOUND, "Setting 'max_block_size' cannot be zero");

    storage_limits.emplace_back(local_limits);

    /// Initialize the initial data streams to which the query transforms are superimposed. Table or subquery or prepared input?
    if (query_plan.isInitialized())
    {
        /// Prepared input.
    }
    else if (interpreter_subquery)
    {
        /// Subquery.
        ASTPtr subquery = extractTableExpression(query, 0);
        if (!subquery)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Subquery expected");

        interpreter_subquery = std::make_unique<InterpreterSelectWithUnionQuery>(
            subquery, getSubqueryContext(context),
            options.copy().subquery().noModify(), required_columns);

        interpreter_subquery->addStorageLimits(storage_limits);

        if (query_analyzer->hasAggregation())
            interpreter_subquery->ignoreWithTotals();

        interpreter_subquery->buildQueryPlan(query_plan);
        query_plan.addInterpreterContext(context);
    }
    else if (storage)
    {
        /// Table.

        /// If necessary, we request more sources than the number of threads - to distribute the work evenly over the threads.
        if (max_streams > 1 && !is_sync_remote)
        {
            if (auto streams_with_ratio = max_streams * settings[Setting::max_streams_to_max_threads_ratio];
                canConvertTo<size_t>(streams_with_ratio))
                max_streams = static_cast<size_t>(streams_with_ratio);
            else
                throw Exception(ErrorCodes::PARAMETER_OUT_OF_BOUND,
                    "Exceeded limit for `max_streams` with `max_streams_to_max_threads_ratio`. "
                    "Make sure that `max_streams * max_streams_to_max_threads_ratio` is in some reasonable boundaries, current value: {}",
                    streams_with_ratio);
        }

        if (max_streams == 0)
            max_streams = 1;

        auto & prewhere_info = analysis_result.prewhere_info;

        if (prewhere_info)
            query_info.prewhere_info = prewhere_info;

        bool optimize_read_in_order = analysis_result.optimize_read_in_order;
        bool optimize_aggregation_in_order = analysis_result.optimize_read_in_order && !query_analyzer->useGroupingSetKey();

        /// Create optimizer with prepared actions.
        /// Maybe we will need to calc input_order_info later, e.g. while reading from StorageMerge.
        if (optimize_read_in_order)
        {
            query_info.order_optimizer = std::make_shared<ReadInOrderOptimizer>(
                query,
                analysis_result.order_by_elements_actions,
                getSortDescription(query, context),
                query_info.syntax_analyzer_result);

            /// If we don't have filtration, we can pushdown limit to reading stage for optimizations.
            UInt64 limit = query.hasFiltration() ? 0 : getLimitForSorting(query, context);
            query_info.input_order_info = query_info.order_optimizer->getInputOrder(metadata_snapshot, context, limit);
        }
        else if (optimize_aggregation_in_order)
        {
            query_info.order_optimizer = std::make_shared<ReadInOrderOptimizer>(
                query,
                analysis_result.group_by_elements_actions,
                getSortDescriptionFromGroupBy(query),
                query_info.syntax_analyzer_result);

            query_info.input_order_info = query_info.order_optimizer->getInputOrder(metadata_snapshot, context, /*limit=*/ 0);
        }

        query_info.storage_limits = std::make_shared<StorageLimitsList>(storage_limits);
        query_info.settings_limit_offset_done = options.settings_limit_offset_done;

        storage->read(query_plan, required_columns, storage_snapshot, query_info, context, processing_stage, max_block_size, max_streams);

        if (context->hasQueryContext() && !options.is_internal)
        {
            auto local_storage_id = storage->getStorageID();
            context->getQueryContext()->addQueryAccessInfo(
                backQuoteIfNeed(local_storage_id.getDatabaseName()),
                local_storage_id.getFullTableName(),
                required_columns);
        }

        /// Create step which reads from empty source if storage has no data.
        if (!query_plan.isInitialized())
        {
            auto header = storage_snapshot->getSampleBlockForColumns(required_columns);
            addEmptySourceToQueryPlan(query_plan, header, query_info);
        }
    }
    else
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Logical error in InterpreterSelectQuery: nowhere to read");

    /// Specify the number of threads only if it wasn't specified in storage.
    ///
    /// But in case of remote query and prefer_localhost_replica=1 (default)
    /// The inner local query (that is done in the same process, without
    /// network interaction), it will setMaxThreads earlier and distributed
    /// query will not update it.
    if (!query_plan.getMaxThreads() || is_sync_remote)
        query_plan.setMaxThreads(max_threads_execute_query);

    query_plan.setConcurrencyControl(settings[Setting::use_concurrency_control]);

    /// Aliases in table declaration.
    if (processing_stage == QueryProcessingStage::FetchColumns && alias_actions)
    {
        auto table_aliases = std::make_unique<ExpressionStep>(query_plan.getCurrentHeader(), alias_actions->clone());
        table_aliases->setStepDescription("Add table aliases");
        query_plan.addStep(std::move(table_aliases));
    }
}

void InterpreterSelectQuery::executeWhere(QueryPlan & query_plan, const ActionsAndProjectInputsFlagPtr & expression, bool remove_filter)
{
    auto dag = expression->dag.clone();
    if (expression->project_input)
        dag.appendInputsForUnusedColumns(*query_plan.getCurrentHeader());

    auto where_step = std::make_unique<FilterStep>(
        query_plan.getCurrentHeader(), std::move(dag), getSelectQuery().where()->getColumnName(), remove_filter);

    where_step->setStepDescription("WHERE");
    query_plan.addStep(std::move(where_step));
}

static Aggregator::Params getAggregatorParams(
    const ASTPtr & query_ptr,
    const SelectQueryExpressionAnalyzer & query_analyzer,
    const Context & context,
    const Names & keys,
    const AggregateDescriptions & aggregates,
    bool overflow_row,
    const Settings & settings,
    size_t group_by_two_level_threshold,
    size_t group_by_two_level_threshold_bytes)
{
    const auto stats_collecting_params = StatsCollectingParams(
        calculateCacheKey(query_ptr),
        settings[Setting::collect_hash_table_stats_during_aggregation],
        context.getServerSettings()[ServerSetting::max_entries_for_hash_table_stats],
        settings[Setting::max_size_to_preallocate_for_aggregation]);

    return Aggregator::Params
    {
        keys,
        aggregates,
        overflow_row,
        settings[Setting::max_rows_to_group_by],
        settings[Setting::group_by_overflow_mode],
        group_by_two_level_threshold,
        group_by_two_level_threshold_bytes,
        Aggregator::Params::getMaxBytesBeforeExternalGroupBy(settings[Setting::max_bytes_before_external_group_by], settings[Setting::max_bytes_ratio_before_external_group_by]),
        settings[Setting::empty_result_for_aggregation_by_empty_set]
            || (settings[Setting::empty_result_for_aggregation_by_constant_keys_on_empty_set] && keys.empty()
                && query_analyzer.hasConstAggregationKeys()),
        context.getTempDataOnDisk(),
        settings[Setting::max_threads],
        settings[Setting::min_free_disk_space_for_temporary_data],
        settings[Setting::compile_aggregate_expressions],
        settings[Setting::min_count_to_compile_aggregate_expression],
        settings[Setting::max_block_size],
        settings[Setting::enable_software_prefetch_in_aggregation],
        /* only_merge */ false,
        settings[Setting::optimize_group_by_constant_keys],
        settings[Setting::min_hit_rate_to_use_consecutive_keys_optimization],
        stats_collecting_params
    };
}

void InterpreterSelectQuery::executeAggregation(
    QueryPlan & query_plan,
    const ActionsAndProjectInputsFlagPtr & expression,
    bool overflow_row,
    bool final,
    InputOrderInfoPtr group_by_info)
{
    executeExpression(query_plan, expression, "Before GROUP BY");

    AggregateDescriptions aggregates = query_analyzer->aggregates();
    const Settings & settings = context->getSettingsRef();
    const auto & keys = query_analyzer->aggregationKeys().getNames();

    auto aggregator_params = getAggregatorParams(
        query_ptr,
        *query_analyzer,
        *context,
        keys,
        aggregates,
        overflow_row,
        settings,
        settings[Setting::group_by_two_level_threshold],
        settings[Setting::group_by_two_level_threshold_bytes]);

    auto grouping_sets_params = getAggregatorGroupingSetsParams(query_analyzer->aggregationKeysList(), keys);

    SortDescription group_by_sort_description;
    SortDescription sort_description_for_merging;

    if (group_by_info && settings[Setting::optimize_aggregation_in_order] && !query_analyzer->useGroupingSetKey())
    {
        group_by_sort_description = getSortDescriptionFromGroupBy(getSelectQuery());
        sort_description_for_merging = group_by_info->sort_description_for_merging;
    }
    else
        group_by_info = nullptr;

    if (!group_by_info && settings[Setting::force_aggregation_in_order])
    {
        group_by_sort_description = getSortDescriptionFromGroupBy(getSelectQuery());
        sort_description_for_merging = group_by_sort_description;
    }

    auto merge_threads = max_streams;
    auto temporary_data_merge_threads = settings[Setting::aggregation_memory_efficient_merge_threads]
        ? static_cast<size_t>(settings[Setting::aggregation_memory_efficient_merge_threads])
        : static_cast<size_t>(settings[Setting::max_threads]);

    bool storage_has_evenly_distributed_read = storage && storage->hasEvenlyDistributedRead();

    const bool should_produce_results_in_order_of_bucket_number = options.to_stage == QueryProcessingStage::WithMergeableState
        && (settings[Setting::distributed_aggregation_memory_efficient] || settings[Setting::enable_memory_bound_merging_of_aggregation_results]);

    auto aggregating_step = std::make_unique<AggregatingStep>(
        query_plan.getCurrentHeader(),
        std::move(aggregator_params),
        std::move(grouping_sets_params),
        final,
        settings[Setting::max_block_size],
        settings[Setting::aggregation_in_order_max_block_bytes],
        merge_threads,
        temporary_data_merge_threads,
        storage_has_evenly_distributed_read,
        settings[Setting::group_by_use_nulls],
        std::move(sort_description_for_merging),
        std::move(group_by_sort_description),
        should_produce_results_in_order_of_bucket_number,
        settings[Setting::enable_memory_bound_merging_of_aggregation_results],
        !group_by_info && settings[Setting::force_aggregation_in_order]);
    query_plan.addStep(std::move(aggregating_step));
}

void InterpreterSelectQuery::executeMergeAggregated(QueryPlan & query_plan, bool overflow_row, bool final, bool has_grouping_sets)
{
    const Settings & settings = context->getSettingsRef();

    const bool should_produce_results_in_order_of_bucket_number = options.to_stage == QueryProcessingStage::WithMergeableState
        && (settings[Setting::distributed_aggregation_memory_efficient] || settings[Setting::enable_memory_bound_merging_of_aggregation_results]);
    const bool parallel_replicas_from_merge_tree = storage->isMergeTree() && context->canUseParallelReplicasOnInitiator();

    executeMergeAggregatedImpl(
        query_plan,
        overflow_row,
        final,
        storage && (storage->isRemote() || parallel_replicas_from_merge_tree),
        has_grouping_sets,
        context->getSettingsRef(),
        query_analyzer->aggregationKeys(),
        query_analyzer->aggregationKeysList(),
        query_analyzer->aggregates(),
        should_produce_results_in_order_of_bucket_number);
}


void InterpreterSelectQuery::executeHaving(QueryPlan & query_plan, const ActionsAndProjectInputsFlagPtr & expression, bool remove_filter)
{
    auto dag = expression->dag.clone();
    if (expression->project_input)
        dag.appendInputsForUnusedColumns(*query_plan.getCurrentHeader());

    auto having_step
        = std::make_unique<FilterStep>(query_plan.getCurrentHeader(), std::move(dag), getSelectQuery().having()->getColumnName(), remove_filter);

    having_step->setStepDescription("HAVING");
    query_plan.addStep(std::move(having_step));
}


void InterpreterSelectQuery::executeTotalsAndHaving(
    QueryPlan & query_plan,
    bool has_having,
    const ActionsAndProjectInputsFlagPtr & expression,
    bool remove_filter,
    bool overflow_row,
    bool final)
{
    std::optional<ActionsDAG> dag;
    if (expression)
    {
        dag = expression->dag.clone();
        if (expression->project_input)
            dag->appendInputsForUnusedColumns(*query_plan.getCurrentHeader());
    }

    const Settings & settings = context->getSettingsRef();

    auto totals_having_step = std::make_unique<TotalsHavingStep>(
        query_plan.getCurrentHeader(),
        query_analyzer->aggregates(),
        overflow_row,
        std::move(dag),
        has_having ? getSelectQuery().having()->getColumnName() : "",
        remove_filter,
        settings[Setting::totals_mode],
        settings[Setting::totals_auto_threshold],
        final);

    query_plan.addStep(std::move(totals_having_step));
}

void InterpreterSelectQuery::executeRollupOrCube(QueryPlan & query_plan, Modificator modificator)
{
    const Settings & settings = context->getSettingsRef();

    const auto & keys = query_analyzer->aggregationKeys().getNames();

    // Arguments will not be present in Rollup / Cube input header and they don't actually needed 'cause these steps will work with AggregateFunctionState-s anyway.
    auto aggregates = query_analyzer->aggregates();
    for (auto & aggregate : aggregates)
        aggregate.argument_names.clear();

    auto params = getAggregatorParams(query_ptr, *query_analyzer, *context, keys, aggregates, false, settings, 0, 0);
    const bool final = true;

    QueryPlanStepPtr step;
    if (modificator == Modificator::ROLLUP)
        step = std::make_unique<RollupStep>(query_plan.getCurrentHeader(), std::move(params), final, settings[Setting::group_by_use_nulls]);
    else if (modificator == Modificator::CUBE)
        step = std::make_unique<CubeStep>(query_plan.getCurrentHeader(), std::move(params), final, settings[Setting::group_by_use_nulls]);

    query_plan.addStep(std::move(step));
}

void InterpreterSelectQuery::executeExpression(QueryPlan & query_plan, const ActionsAndProjectInputsFlagPtr & expression, const std::string & description)
{
    if (!expression)
        return;

    auto dag = expression->dag.clone();
    if (expression->project_input)
        dag.appendInputsForUnusedColumns(*query_plan.getCurrentHeader());

    auto expression_step = std::make_unique<ExpressionStep>(query_plan.getCurrentHeader(), std::move(dag));

    expression_step->setStepDescription(description);
    query_plan.addStep(std::move(expression_step));
}

static bool windowDescriptionComparator(const WindowDescription * _left, const WindowDescription * _right)
{
    const auto & left = _left->full_sort_description;
    const auto & right = _right->full_sort_description;

    for (size_t i = 0; i < std::min(left.size(), right.size()); ++i)
    {
        if (left[i].column_name < right[i].column_name)
            return true;
        if (left[i].column_name > right[i].column_name)
            return false;
        if (left[i].direction < right[i].direction)
            return true;
        if (left[i].direction > right[i].direction)
            return false;
        if (left[i].nulls_direction < right[i].nulls_direction)
            return true;
        if (left[i].nulls_direction > right[i].nulls_direction)
            return false;

        assert(left[i] == right[i]);
    }

    // Note that we check the length last, because we want to put together the
    // sort orders that have common prefix but different length.
    return left.size() > right.size();
}

static bool sortIsPrefix(const WindowDescription & _prefix,
    const WindowDescription & _full)
{
    const auto & prefix = _prefix.full_sort_description;
    const auto & full = _full.full_sort_description;

    if (prefix.size() > full.size())
        return false;

    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (full[i] != prefix[i])
            return false;
    }

    return true;
}

void InterpreterSelectQuery::executeWindow(QueryPlan & query_plan)
{
    // Try to sort windows in such an order that the window with the longest
    // sort description goes first, and all window that use its prefixes follow.
    std::vector<const WindowDescription *> windows_sorted;
    for (const auto & [_, window] : query_analyzer->windowDescriptions())
        windows_sorted.push_back(&window);

    ::sort(windows_sorted.begin(), windows_sorted.end(), windowDescriptionComparator);

    const Settings & settings = context->getSettingsRef();
    for (size_t i = 0; i < windows_sorted.size(); ++i)
    {
        const auto & window = *windows_sorted[i];

        // We don't need to sort again if the input from previous window already
        // has suitable sorting. Also don't create sort steps when there are no
        // columns to sort by, because the sort nodes are confused by this. It
        // happens in case of `over ()`.
        // Even if full_sort_description of both windows match, in case of different
        // partitioning we need to add a SortingStep to reshuffle data in the streams.
        bool need_sort = !window.full_sort_description.empty();
        if (need_sort && i != 0)
        {
            need_sort = !sortIsPrefix(window, *windows_sorted[i - 1])
                || (settings[Setting::max_threads] != 1 && window.partition_by.size() != windows_sorted[i - 1]->partition_by.size());
        }
        if (need_sort)
        {
            SortingStep::Settings sort_settings(context->getSettingsRef());

            auto sorting_step = std::make_unique<SortingStep>(
                query_plan.getCurrentHeader(),
                window.full_sort_description,
                window.partition_by,
                0 /* LIMIT */,
                sort_settings);
            sorting_step->setStepDescription("Sorting for window '" + window.window_name + "'");
            query_plan.addStep(std::move(sorting_step));
        }

        // Fan out streams only for the last window to preserve the ordering between windows,
        // and WindowTransform works on single stream anyway.
        const bool streams_fan_out
            = settings[Setting::query_plan_enable_multithreading_after_window_functions] && ((i + 1) == windows_sorted.size());

        auto window_step = std::make_unique<WindowStep>(query_plan.getCurrentHeader(), window, window.window_functions, streams_fan_out);
        window_step->setStepDescription("Window step for window '" + window.window_name + "'");

        query_plan.addStep(std::move(window_step));
    }
}


void InterpreterSelectQuery::executeOrderOptimized(QueryPlan & query_plan, InputOrderInfoPtr input_sorting_info, UInt64 limit, SortDescription & output_order_descr)
{
    const Settings & settings = context->getSettingsRef();

    auto finish_sorting_step = std::make_unique<SortingStep>(
        query_plan.getCurrentHeader(),
        input_sorting_info->sort_description_for_merging,
        output_order_descr,
        settings[Setting::max_block_size],
        limit);

    query_plan.addStep(std::move(finish_sorting_step));
}

void InterpreterSelectQuery::executeOrder(QueryPlan & query_plan, InputOrderInfoPtr input_sorting_info)
{
    auto & query = getSelectQuery();
    SortDescription output_order_descr = getSortDescription(query, context);
    UInt64 limit = getLimitForSorting(query, context);

    if (input_sorting_info)
    {
        /* Case of sorting with optimization using sorting key.
         * We have several threads, each of them reads batch of parts in direct
         *  or reverse order of sorting key using one input stream per part
         *  and then merge them into one sorted stream.
         * At this stage we merge per-thread streams into one.
         */
        executeOrderOptimized(query_plan, input_sorting_info, limit, output_order_descr);
        return;
    }

    SortingStep::Settings sort_settings(context->getSettingsRef());

    /// Merge the sorted blocks.
    auto sorting_step = std::make_unique<SortingStep>(
        query_plan.getCurrentHeader(),
        output_order_descr,
        limit,
        sort_settings);

    sorting_step->setStepDescription("Sorting for ORDER BY");
    query_plan.addStep(std::move(sorting_step));
}


void InterpreterSelectQuery::executeMergeSorted(QueryPlan & query_plan, const std::string & description)
{
    const auto & query = getSelectQuery();
    SortDescription sort_description = getSortDescription(query, context);
    const UInt64 limit = getLimitForSorting(query, context);
    const auto max_block_size = context->getSettingsRef()[Setting::max_block_size];
    const auto exact_rows_before_limit = context->getSettingsRef()[Setting::exact_rows_before_limit];

    auto merging_sorted = std::make_unique<SortingStep>(
        query_plan.getCurrentHeader(), std::move(sort_description), max_block_size, limit, exact_rows_before_limit);
    merging_sorted->setStepDescription("Merge sorted streams " + description);
    query_plan.addStep(std::move(merging_sorted));
}


void InterpreterSelectQuery::executeProjection(QueryPlan & query_plan, const ActionsAndProjectInputsFlagPtr & expression)
{
    executeExpression(query_plan, expression, "Projection");
}


void InterpreterSelectQuery::executeDistinct(QueryPlan & query_plan, bool before_order, Names columns, bool pre_distinct)
{
    auto & query = getSelectQuery();
    if (query.distinct)
    {
        const Settings & settings = context->getSettingsRef();

        UInt64 limit_for_distinct = 0;

        /// If after this stage of DISTINCT,
        /// (1) ORDER BY is not executed
        /// (2) there is no LIMIT BY (todo: we can check if DISTINCT and LIMIT BY expressions are match)
        /// then you can get no more than limit_length + limit_offset of different rows.
        if ((!query.orderBy() || !before_order) && !query.limitBy())
        {
            auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);
            if (limit_length <= std::numeric_limits<UInt64>::max() - limit_offset)
                limit_for_distinct = limit_length + limit_offset;
        }

        SizeLimits limits(settings[Setting::max_rows_in_distinct], settings[Setting::max_bytes_in_distinct], settings[Setting::distinct_overflow_mode]);

        auto distinct_step = std::make_unique<DistinctStep>(
            query_plan.getCurrentHeader(),
            limits,
            limit_for_distinct,
            columns,
            pre_distinct);

        if (pre_distinct)
            distinct_step->setStepDescription("Preliminary DISTINCT");

        query_plan.addStep(std::move(distinct_step));
    }
}


/// Preliminary LIMIT - is used in every source, if there are several sources, before they are combined.
void InterpreterSelectQuery::executePreLimit(QueryPlan & query_plan, bool do_not_skip_offset)
{
    auto & query = getSelectQuery();
    /// If there is LIMIT
    if (query.limitLength())
    {
        auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);

        if (do_not_skip_offset)
        {
            if (limit_length > std::numeric_limits<UInt64>::max() - limit_offset)
                return;

            limit_length += limit_offset;
            limit_offset = 0;
        }

        const Settings & settings = context->getSettingsRef();

        auto limit
            = std::make_unique<LimitStep>(query_plan.getCurrentHeader(), limit_length, limit_offset, settings[Setting::exact_rows_before_limit]);
        if (do_not_skip_offset)
            limit->setStepDescription("preliminary LIMIT (with OFFSET)");
        else
            limit->setStepDescription("preliminary LIMIT (without OFFSET)");

        query_plan.addStep(std::move(limit));
    }
}


void InterpreterSelectQuery::executeLimitBy(QueryPlan & query_plan)
{
    auto & query = getSelectQuery();
    if (!query.limitByLength() || !query.limitBy())
        return;

    Names columns;
    for (const auto & elem : query.limitBy()->children)
        columns.emplace_back(elem->getColumnName());

    UInt64 length = getLimitUIntValue(query.limitByLength(), context, "LIMIT");
    UInt64 offset = (query.limitByOffset() ? getLimitUIntValue(query.limitByOffset(), context, "OFFSET") : 0);

    auto limit_by = std::make_unique<LimitByStep>(query_plan.getCurrentHeader(), length, offset, columns);
    query_plan.addStep(std::move(limit_by));
}

void InterpreterSelectQuery::executeWithFill(QueryPlan & query_plan)
{
    auto & query = getSelectQuery();
    if (query.orderBy())
    {
        SortDescription sort_description = getSortDescription(query, context);
        SortDescription fill_description;
        for (auto & desc : sort_description)
        {
            if (desc.with_fill)
                fill_description.push_back(desc);
        }

        if (fill_description.empty())
            return;

        InterpolateDescriptionPtr interpolate_descr =
            getInterpolateDescription(query, *source_header, *result_header, syntax_analyzer_result->aliases, context);

        const Settings & settings = context->getSettingsRef();
        auto filling_step = std::make_unique<FillingStep>(
            query_plan.getCurrentHeader(),
            std::move(sort_description),
            std::move(fill_description),
            interpolate_descr,
            settings[Setting::use_with_fill_by_sorting_prefix]);
        query_plan.addStep(std::move(filling_step));
    }
}


void InterpreterSelectQuery::executeLimit(QueryPlan & query_plan)
{
    auto & query = getSelectQuery();
    /// If there is LIMIT
    if (query.limitLength())
    {
        /** Rare case:
          *  if there is no WITH TOTALS and there is a subquery in FROM, and there is WITH TOTALS on one of the levels,
          *  then when using LIMIT, you should read the data to the end, rather than cancel the query earlier,
          *  because if you cancel the query, we will not get `totals` data from the remote server.
          *
          * Another case:
          *  if there is WITH TOTALS and there is no ORDER BY, then read the data to the end,
          *  otherwise TOTALS is counted according to incomplete data.
          */
        const Settings & settings = context->getSettingsRef();
        bool always_read_till_end = settings[Setting::exact_rows_before_limit];

        if (query.group_by_with_totals && !query.orderBy())
            always_read_till_end = true;

        if (!query.group_by_with_totals && hasWithTotalsInAnySubqueryInFromClause(query))
            always_read_till_end = true;

        UInt64 limit_length;
        UInt64 limit_offset;
        std::tie(limit_length, limit_offset) = getLimitLengthAndOffset(query, context);

        SortDescription order_descr;
        if (query.limit_with_ties)
        {
            if (!query.orderBy())
                throw Exception(ErrorCodes::LOGICAL_ERROR, "LIMIT WITH TIES without ORDER BY");
            order_descr = getSortDescription(query, context);
        }

        auto limit = std::make_unique<LimitStep>(
                query_plan.getCurrentHeader(),
                limit_length, limit_offset, always_read_till_end, query.limit_with_ties, order_descr);

        if (query.limit_with_ties)
            limit->setStepDescription("LIMIT WITH TIES");

        query_plan.addStep(std::move(limit));
    }
}


void InterpreterSelectQuery::executeOffset(QueryPlan & query_plan)
{
    auto & query = getSelectQuery();
    /// If there is not a LIMIT but an offset
    if (!query.limitLength() && query.limitOffset())
    {
        UInt64 limit_length;
        UInt64 limit_offset;
        std::tie(limit_length, limit_offset) = getLimitLengthAndOffset(query, context);

        auto offsets_step = std::make_unique<OffsetStep>(query_plan.getCurrentHeader(), limit_offset);
        query_plan.addStep(std::move(offsets_step));
    }
}

void InterpreterSelectQuery::executeExtremes(QueryPlan & query_plan)
{
    if (!context->getSettingsRef()[Setting::extremes])
        return;

    auto extremes_step = std::make_unique<ExtremesStep>(query_plan.getCurrentHeader());
    query_plan.addStep(std::move(extremes_step));
}

void InterpreterSelectQuery::executeSubqueriesInSetsAndJoins(QueryPlan & query_plan)
{
    auto subqueries = prepared_sets->getSubqueries();

    if (!subqueries.empty())
    {
        const auto & settings = context->getSettingsRef();
        SizeLimits network_transfer_limits(settings[Setting::max_rows_to_transfer], settings[Setting::max_bytes_to_transfer], settings[Setting::transfer_overflow_mode]);
        auto prepared_sets_cache = context->getPreparedSetsCache();

        auto step = std::make_unique<DelayedCreatingSetsStep>(
                query_plan.getCurrentHeader(),
                std::move(subqueries),
                network_transfer_limits,
                prepared_sets_cache);

        query_plan.addStep(std::move(step));
    }
}


void InterpreterSelectQuery::ignoreWithTotals()
{
    getSelectQuery().group_by_with_totals = false;
}

bool InterpreterSelectQuery::autoFinalOnQuery(ASTSelectQuery & query)
{
    // query.tables() is required because not all queries have tables in it, it could be a function.
    bool is_auto_final_setting_on = context->getSettingsRef()[Setting::final];
    bool is_final_supported = storage && storage->supportsFinal() && !storage->isRemote() && query.tables();
    bool is_query_already_final = query.final();

    return is_auto_final_setting_on && !is_query_already_final && is_final_supported;
}

void InterpreterSelectQuery::initSettings()
{
    auto & query = getSelectQuery();
    if (query.settings())
        InterpreterSetQuery(query.settings(), context).executeForCurrentContext(options.ignore_setting_constraints);

    const auto & client_info = context->getClientInfo();
    auto min_major = DBMS_MIN_MAJOR_VERSION_WITH_CURRENT_AGGREGATION_VARIANT_SELECTION_METHOD;
    auto min_minor = DBMS_MIN_MINOR_VERSION_WITH_CURRENT_AGGREGATION_VARIANT_SELECTION_METHOD;

    if (client_info.query_kind == ClientInfo::QueryKind::SECONDARY_QUERY &&
        std::forward_as_tuple(client_info.connection_client_version_major, client_info.connection_client_version_minor) < std::forward_as_tuple(min_major, min_minor))
    {
        /// Disable two-level aggregation due to version incompatibility.
        context->setSetting("group_by_two_level_threshold", Field(0));
        context->setSetting("group_by_two_level_threshold_bytes", Field(0));

    }
}

bool InterpreterSelectQuery::isQueryWithFinal(const SelectQueryInfo & info)
{
    bool result = info.query->as<ASTSelectQuery &>().final();
    if (info.table_expression_modifiers)
        result |= info.table_expression_modifiers->hasFinal();

    return result;
}

void registerInterpreterSelectQuery(InterpreterFactory & factory)
{
    auto create_fn = [] (const InterpreterFactory::Arguments & args)
    {
        return std::make_unique<InterpreterSelectQuery>(args.query, args.context, args.options);
    };
    factory.registerInterpreter("InterpreterSelectQuery", create_fn);
}

}
