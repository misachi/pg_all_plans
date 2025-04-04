#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/pg_proc.h"
#include "commands/explain.h"
#include "executor/instrument.h"
#include "executor/tstoreReceiver.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/subselect.h"
#include "parser/parse_node.h"
#include "partitioning/partdesc.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

static MemoryContext ShowAllPlanContext = NULL;

static void explain_query(Query *query, int cursorOptions,
                          IntoClause *into, ExplainState *es,
                          const char *queryString, ParamListInfo params,
                          QueryEnvironment *queryEnv);
static PlannedStmt *get_planned_stmt(
    PlannerInfo *root, PlannerGlobal *glob, Query *parse,
    Path *path, double tuple_fraction, int cursorOptions);

Datum show_all_plans(PG_FUNCTION_ARGS);

static PlannedStmt *get_planned_stmt(
    PlannerInfo *root, PlannerGlobal *glob, Query *parse,
    Path *path, double tuple_fraction, int cursorOptions)
{
    PlannedStmt *result;
    Plan *top_plan;
    ListCell *lp, *lr;

    top_plan = create_plan(root, path);

    if (cursorOptions & CURSOR_OPT_SCROLL)
    {
        if (!ExecSupportsBackwardScan(top_plan))
            top_plan = materialize_finished_plan(top_plan);
    }

    if (debug_parallel_query != DEBUG_PARALLEL_OFF &&
        top_plan->parallel_safe &&
        (top_plan->initPlan == NIL ||
         debug_parallel_query != DEBUG_PARALLEL_REGRESS))
    {
        Gather *gather = makeNode(Gather);
        Cost initplan_cost;
        bool unsafe_initplans;

        gather->plan.targetlist = top_plan->targetlist;
        gather->plan.qual = NIL;
        gather->plan.lefttree = top_plan;
        gather->plan.righttree = NULL;
        gather->num_workers = 1;
        gather->single_copy = true;
        gather->invisible = (debug_parallel_query == DEBUG_PARALLEL_REGRESS);

        /* Transfer any initPlans to the new top node */
        gather->plan.initPlan = top_plan->initPlan;
        top_plan->initPlan = NIL;

        gather->rescan_param = -1;

        gather->plan.startup_cost = top_plan->startup_cost +
                                    parallel_setup_cost;
        gather->plan.total_cost = top_plan->total_cost +
                                  parallel_setup_cost + parallel_tuple_cost * top_plan->plan_rows;
        gather->plan.plan_rows = top_plan->plan_rows;
        gather->plan.plan_width = top_plan->plan_width;
        gather->plan.parallel_aware = false;
        gather->plan.parallel_safe = false;

        SS_compute_initplan_cost(gather->plan.initPlan,
                                 &initplan_cost, &unsafe_initplans);
        top_plan->startup_cost -= initplan_cost;
        top_plan->total_cost -= initplan_cost;

        /* use parallel mode for parallel plans. */
        root->glob->parallelModeNeeded = true;

        top_plan = &gather->plan;
    }

    if (glob->paramExecTypes != NIL)
    {
        Assert(list_length(glob->subplans) == list_length(glob->subroots));
        forboth(lp, glob->subplans, lr, glob->subroots)
        {
            Plan *subplan = (Plan *)lfirst(lp);
            PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

            SS_finalize_plan(subroot, subplan);
        }
        SS_finalize_plan(root, top_plan);
    }

    /* final cleanup of the plan */
    Assert(glob->finalrtable == NIL);
    Assert(glob->finalrteperminfos == NIL);
    Assert(glob->finalrowmarks == NIL);
    Assert(glob->resultRelations == NIL);
    Assert(glob->appendRelations == NIL);
    top_plan = set_plan_references(root, top_plan);
    /* ... and the subplans (both regular subplans and initplans) */
    Assert(list_length(glob->subplans) == list_length(glob->subroots));
    forboth(lp, glob->subplans, lr, glob->subroots)
    {
        Plan *subplan = (Plan *)lfirst(lp);
        PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

        lfirst(lp) = set_plan_references(subroot, subplan);
    }

    /* build the PlannedStmt result */
    result = makeNode(PlannedStmt);

    result->commandType = parse->commandType;
    result->queryId = parse->queryId;
    result->hasReturning = (parse->returningList != NIL);
    result->hasModifyingCTE = parse->hasModifyingCTE;
    result->canSetTag = parse->canSetTag;
    result->transientPlan = glob->transientPlan;
    result->dependsOnRole = glob->dependsOnRole;
    result->parallelModeNeeded = glob->parallelModeNeeded;
    result->planTree = top_plan;
    result->rtable = glob->finalrtable;
    result->permInfos = glob->finalrteperminfos;
    result->resultRelations = glob->resultRelations;
    result->appendRelations = glob->appendRelations;
    result->subplans = glob->subplans;
    result->rewindPlanIDs = glob->rewindPlanIDs;
    result->rowMarks = glob->finalrowmarks;
    result->relationOids = glob->relationOids;
    result->invalItems = glob->invalItems;
    result->paramExecTypes = glob->paramExecTypes;
    /* utilityStmt should be null, but we might as well copy it */
    result->utilityStmt = parse->utilityStmt;
    result->stmt_location = parse->stmt_location;
    result->stmt_len = parse->stmt_len;

    result->jitFlags = PGJIT_NONE;
    if (jit_enabled && jit_above_cost >= 0 &&
        top_plan->total_cost > jit_above_cost)
    {
        result->jitFlags |= PGJIT_PERFORM;

        if (jit_optimize_above_cost >= 0 &&
            top_plan->total_cost > jit_optimize_above_cost)
            result->jitFlags |= PGJIT_OPT3;
        if (jit_inline_above_cost >= 0 &&
            top_plan->total_cost > jit_inline_above_cost)
            result->jitFlags |= PGJIT_INLINE;

        if (jit_expressions)
            result->jitFlags |= PGJIT_EXPR;
        if (jit_tuple_deforming)
            result->jitFlags |= PGJIT_DEFORM;
    }

    if (glob->partition_directory != NULL)
        DestroyPartitionDirectory(glob->partition_directory);

    return result;
}

static void explain_query(Query *query, int cursorOptions,
                          IntoClause *into, ExplainState *es,
                          const char *queryString, ParamListInfo params,
                          QueryEnvironment *queryEnv)
{
    instr_time planstart,
        planduration, temp_duration;
    BufferUsage bufusage_start,
        bufusage;
    ListCell *l;
    int k = 1;
    PlannerGlobal *glob;
    double tuple_fraction;
    PlannerInfo *root;
    RelOptInfo *final_rel;

    if (es->buffers)
        bufusage_start = pgBufferUsage;
    INSTR_TIME_SET_CURRENT(planstart);

    /* plan the query */
    glob = makeNode(PlannerGlobal);

    glob->boundParams = params;

    if ((cursorOptions & CURSOR_OPT_PARALLEL_OK) != 0 &&
        IsUnderPostmaster &&
        query->commandType == CMD_SELECT &&
        !query->hasModifyingCTE &&
        max_parallel_workers_per_gather > 0 &&
        !IsParallelWorker())
    {
        /* all the cheap tests pass, so scan the query tree */
        glob->maxParallelHazard = max_parallel_hazard(query);
        glob->parallelModeOK = (glob->maxParallelHazard != PROPARALLEL_UNSAFE);
    }
    else
    {
        /* skip the query tree scan, just assume it's unsafe */
        glob->maxParallelHazard = PROPARALLEL_UNSAFE;
        glob->parallelModeOK = false;
    }

    glob->parallelModeNeeded = glob->parallelModeOK &&
                               (debug_parallel_query != DEBUG_PARALLEL_OFF);

    /* Determine what fraction of the plan is likely to be scanned */
    if (cursorOptions & CURSOR_OPT_FAST_PLAN)
    {
        tuple_fraction = cursor_tuple_fraction;

        if (tuple_fraction >= 1.0)
            tuple_fraction = 0.0;
        else if (tuple_fraction <= 0.0)
            tuple_fraction = 1e-10;
    }
    else
    {
        /* Default assumption is we need all the tuples */
        tuple_fraction = 0.0;
    }

    /* primary planning entry point (may recurse for subqueries) */
#if PG_MAJORVERSION_NUM <= 17 && PG_MINORVERSION_NUM < 4
    root = subquery_planner(glob, query, NULL,
                            false, tuple_fraction);
#else
    root = subquery_planner(glob, query, NULL,
                            false, tuple_fraction, NULL);
#endif
    /* Select best Path and turn it into a Plan */
    final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);

    INSTR_TIME_SET_CURRENT(temp_duration);
    INSTR_TIME_SUBTRACT(temp_duration, planstart);

    foreach (l, final_rel->pathlist)
    {
        Path *path = (Path *)lfirst(l);
        PlannedStmt *plan;

        INSTR_TIME_SET_CURRENT(planstart);
        plan = get_planned_stmt(root, glob, query, path, tuple_fraction, cursorOptions);

        INSTR_TIME_SET_CURRENT(planduration);
        INSTR_TIME_SUBTRACT(planduration, planstart);
        INSTR_TIME_ADD(planduration, temp_duration);

        if (es->buffers)
        {
            memset(&bufusage, 0, sizeof(BufferUsage));
            BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);
        }

        appendStringInfo(es->str, "-------------------------------Plan %d-------------------------------\n", k++);

#if PG_MAJORVERSION_NUM <= 17 && PG_MINORVERSION_NUM < 4
        ExplainOnePlan(plan, into, es, queryString, params, queryEnv,
                       &planduration, (es->buffers ? &bufusage : NULL));
#else
        ExplainOnePlan(plan, into, es, queryString, params, queryEnv,
                       &planduration, (es->buffers ? &bufusage : NULL), NULL);
#endif
        ExplainSeparatePlans(es);
    }
}

PG_FUNCTION_INFO_V1(show_all_plans);
Datum show_all_plans(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
    char *query_string = text_to_cstring(PG_GETARG_TEXT_P(0));
    ParseState *pstate;
    DestReceiver *receiver;
    List *parsetree_list, *plantree_list, *querytree_list;
    PlannedStmt *stmt;
    RawStmt *parsetree;
    Tuplestorestate *tupstore;
    MemoryContext oldctx;
    TupleDesc tupdesc;
    ListCell *lc;
    bool randomAccess;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("materialize mode required, but it is not allowed in this context")));

    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = query_string;
    pstate->p_queryEnv = NULL;

    ShowAllPlanContext = rsinfo->econtext->ecxt_per_query_memory;
    // AllocSetContextCreate(TopMemoryContext,
    //                                            "ShowAllPlanContext",
    //                                            ALLOCSET_DEFAULT_SIZES);
    oldctx = MemoryContextSwitchTo(ShowAllPlanContext);

    randomAccess = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;

    tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);

    MemoryContextSwitchTo(oldctx);

    /* Prep the destination receiver */
    receiver = CreateDestReceiver(DestTuplestore);
    SetTuplestoreDestReceiverParams(receiver,
                                    tupstore,
                                    ShowAllPlanContext,
                                    false,
                                    NULL,
                                    NULL);

    parsetree_list = pg_parse_query(query_string);

    parsetree = linitial_node(RawStmt, parsetree_list);

    if (nodeTag(parsetree->stmt) != T_ExplainStmt)
    {
        receiver->rDestroy(receiver);
        elog(ERROR, "Only EXPLAIN utility statements are allowed");
    }

    querytree_list = pg_analyze_and_rewrite_fixedparams(parsetree, query_string, NULL, 0, NULL);
    plantree_list = pg_plan_queries(querytree_list, query_string, CURSOR_OPT_PARALLEL_OK, NULL);

    stmt = NULL;

    /* Get the "primary" stmt within a portal, ie, the one marked canSetTag. */
    foreach (lc, plantree_list)
    {
        PlannedStmt *stmt2 = lfirst_node(PlannedStmt, lc);

        if (stmt2->canSetTag)
            stmt = stmt2;
    }

    Assert(stmt->commandType == CMD_UTILITY);

    /* Temporarily set expain hook
     * Here the tuple store will be populated with the necessary tuples
     */
    ExplainOneQuery_hook = explain_query;
    ExplainQuery(pstate, (ExplainStmt *)stmt->utilityStmt, NULL, receiver);
    ExplainOneQuery_hook = NULL;

    {
        MemoryContext oldcontext;

        oldcontext = MemoryContextSwitchTo(ShowAllPlanContext);
        tupdesc = CreateTemplateTupleDesc(1);
        TupleDescInitEntry(tupdesc, (AttrNumber)1, "QUERY PLAN", TEXTOID, -1, 0);

        rsinfo->returnMode = SFRM_Materialize;

        /* We already have our tuple, so send it to the return set */
        rsinfo->setResult = tupstore;
        rsinfo->setDesc = tupdesc;
        MemoryContextSwitchTo(oldcontext);
    }

    receiver->rDestroy(receiver);
    PG_RETURN_NULL();
}