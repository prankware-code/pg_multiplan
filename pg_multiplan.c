#include "postgres.h"
#include "access/table.h"
#include "access/heapam.h"
#include "catalog/namespace.h"
#include "fmgr.h"
#include "funcapi.h"
#include "optimizer/planner.h"
#include "nodes/makefuncs.h"
#include "nodes/params.h"
#include "nodes/queryjumble.h"
#include "nodes/pg_list.h"
#include "nodes/nodes.h"
#include "miscadmin.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/guc.h"

#include <stdbool.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_multiplan_list);
PG_FUNCTION_INFO_V1(pg_multiplan_select);

static MemoryContext plan_cache_ctx = NULL;
static planner_hook_type prev_planner_hook = NULL;
static HTAB *hashed_plans = NULL;
static uint64 last_query_id = 0;
static bool cache_loaded = false;

// guc tables
static bool enable_caching = false;
static bool enable_contrib = false;

typedef struct PlanHashEntry
{
	uint64 queryId;
	List *plans_list;
	char *query;
	int64 selected;
} PlanHashEntry;

void _PG_init(void);
void _PG_fini(void);
PlannedStmt *
multiplan_planner_hook(Query *parse,
					   const char *query_string,
					   int cursorOptions,
					   ParamListInfo boundParams,
					   ExplainState *es);

static void
load_plans_from_table(void)
{
    RangeVar      *rv;
    Oid            relid;
    Relation       rel;
    TableScanDesc  scan;
    HeapTuple      tup;
    TupleDesc      tupdesc;
    MemoryContext  oldctx;

    rv = makeRangeVar(NULL, "pg_multiplan_plans", -1);
    relid = RangeVarGetRelid(rv, AccessShareLock, true);

    if (!OidIsValid(relid))
        return;

    rel = table_open(relid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    scan = table_beginscan_catalog(rel, 0, NULL);

    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        Datum   d_qid, d_qtext, d_pdata;
		// Datum d_plan_id;
        bool    isnull;
        uint64  query_id;
        char   *plan_str, *query_str;
        PlannedStmt *plan;
        PlanHashEntry *entry;
        bool    found;

        d_qid   = heap_getattr(tup, 1, tupdesc, &isnull);
        // d_plan_id = heap_getattr(tup, 2, tupdesc, &isnull);
        d_qtext = heap_getattr(tup, 3, tupdesc, &isnull);
        d_pdata = heap_getattr(tup, 4, tupdesc, &isnull);

        query_id = strtoull(text_to_cstring(DatumGetTextPP(d_qid)), NULL, 10);
        plan_str = text_to_cstring(DatumGetTextPP(d_pdata));
        query_str = text_to_cstring(DatumGetTextPP(d_qtext));

        oldctx = MemoryContextSwitchTo(plan_cache_ctx);

        plan = (PlannedStmt *) stringToNode(plan_str);

        entry = hash_search(hashed_plans, &query_id, HASH_ENTER, &found);
        if (!found)
        {
            entry->plans_list = NIL;
            entry->selected = 1;
            entry->query = pstrdup(query_str);
        }
        entry->plans_list = lappend(entry->plans_list, plan);

        MemoryContextSwitchTo(oldctx);
    }

    table_endscan(scan);
    table_close(rel, AccessShareLock);
}

static void
persist_plan(uint64 query_id, int64 plan_id,
             const char *query_text, const char *plan_data)
{
    RangeVar   *rv;
    Oid         relid;
    Relation    rel;
    Datum       values[5];
    bool        nulls[5] = {false, false, false, false, false};
    HeapTuple   tup;
    char        qid_buf[32];

    snprintf(qid_buf, sizeof(qid_buf), UINT64_FORMAT, query_id);

    rv = makeRangeVar(NULL, "pg_multiplan_plans", -1);
    relid = RangeVarGetRelid(rv, RowExclusiveLock, false);

    rel = table_open(relid, RowExclusiveLock);

    values[0] = CStringGetTextDatum(qid_buf);
    values[1] = Int64GetDatum(plan_id);
    values[2] = CStringGetTextDatum(query_text);
    values[3] = CStringGetTextDatum(plan_data);
    values[4] = BoolGetDatum(false);

    tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

    simple_heap_insert(rel, tup);

    heap_freetuple(tup);
    table_close(rel, RowExclusiveLock);
}

static void
ensure_memory_ctx(void)
{
	if (!plan_cache_ctx)
		plan_cache_ctx = AllocSetContextCreate(TopMemoryContext, "pg_multiplan cache", ALLOCSET_DEFAULT_SIZES);
}

static void
plan_cache_destroy(void)
{
	if (hashed_plans)
	{
		hash_destroy(hashed_plans);
		hashed_plans = NULL;
	}

    if (plan_cache_ctx)
    {
    	MemoryContextReset(plan_cache_ctx);
		plan_cache_ctx = NULL;
    }
}

static PlannedStmt *
get_standart_plan(Query *parse,
				  const char *query_string,
			      int cursorOptions,
				  ParamListInfo boundParams,
				  ExplainState *es)
{
	if (prev_planner_hook)
		return prev_planner_hook(parse, query_string, cursorOptions, boundParams, es);

	return standard_planner(parse, query_string, cursorOptions, boundParams, es);
}

Datum
pg_multiplan_list(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc tupledesc;
	Tuplestorestate *tupstore;
	MemoryContext  per_query_ctx;
	MemoryContext  oldcontext;
	uint64	filter = 0;
	int64 plan_id = 1;
	PlanHashEntry *hash_entry;
	ListCell *lc;
	bool hash_found = false;

	if (!enable_contrib)
		return (Datum) 0;

	if (PG_NARGS() >= 1 && !PG_ARGISNULL(0))
		filter = PG_GETARG_INT64(0);

	if (filter == 0 && last_query_id == 0)
		ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("use pg_multiplan_list(queryId) or execute a preflight query")));

	if (filter == 0)
		filter = last_query_id;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
	
	if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not allowed in this context")));
	
	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));
	
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupledesc;

	MemoryContextSwitchTo(oldcontext);

	hash_entry = (PlanHashEntry *) hash_search(hashed_plans, &filter, HASH_FIND, &hash_found);

	if (!hash_found)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("use enable_caching for save plans")));

	foreach(lc, hash_entry->plans_list)
	{
		PlannedStmt *entry = (PlannedStmt *) lfirst(lc);
		Datum	values[5];
		bool	nulls[5] = {false, false, false, false, false};

		bool	is_selected = (hash_entry->selected == plan_id);
		char	qid_buf[32];
		snprintf(qid_buf, sizeof(qid_buf), UINT64_FORMAT, (uint64) hash_entry->queryId);

		values[0] = Int64GetDatum(plan_id++);
		values[1] = CStringGetTextDatum(qid_buf);
		values[2] = Float8GetDatum(entry->planTree->total_cost);
		values[3] = CStringGetTextDatum(hash_entry->query);
		values[4] = BoolGetDatum(is_selected);

		tuplestore_putvalues(tupstore, tupledesc, values, nulls);
	}

	return (Datum) 0;
}

Datum
pg_multiplan_select(PG_FUNCTION_ARGS)
{
	uint64 query_id = strtoull(text_to_cstring(PG_GETARG_TEXT_PP(0)), NULL, 10);
	int64 plan_id = PG_GETARG_INT64(1);
	bool found = false;
	PlanHashEntry *hash_entry = NULL;

	if (!enable_contrib)
		PG_RETURN_VOID();

	hash_entry = (PlanHashEntry *) hash_search(hashed_plans, &query_id, HASH_FIND, &found);

	if (!found)
		ereport(ERROR,
			(errcode(ERRCODE_NO_DATA_FOUND),
				errmsg("query id was not found")));
	
	if (plan_id > hash_entry->plans_list->length)
		ereport(ERROR,
			(errcode(ERRCODE_NO_DATA_FOUND),
				errmsg("plan id was not found")));
	
	hash_entry->selected = plan_id;

	PG_RETURN_VOID();
}

PlannedStmt *
multiplan_planner_hook(Query *parse,
					   const char *query_string,
					   int cursorOptions,
					   ParamListInfo boundParams,
					   ExplainState *es)
{
	MemoryContext oldctx = NULL;
	PlanHashEntry *hash_entry = NULL;
	bool hash_found = false;
	PlannedStmt *result = NULL;

	if (enable_contrib && !cache_loaded)
    {
        load_plans_from_table();
        cache_loaded = true;
    }

	if (parse->queryId  == INT64CONST(0))
		JumbleQuery(parse);

	elog(NOTICE, "query = %s, query_id = %lu", query_string, parse->queryId);

	oldctx = MemoryContextSwitchTo(plan_cache_ctx);

	hash_entry = (PlanHashEntry *) hash_search(hashed_plans, &parse->queryId, HASH_ENTER, &hash_found);

	if (!hash_found)
	{
		hash_entry->plans_list = NIL;
		hash_entry->selected = 1;
		hash_entry->query = NULL;
	}

	if (enable_contrib && enable_caching)
	{
		char *serialized_plan = NULL;
		result = get_standart_plan(parse, query_string, cursorOptions, boundParams, es);

		hash_entry->plans_list = lappend(hash_entry->plans_list, copyObject(result));
		hash_entry->query = pstrdup(query_string);

		serialized_plan = nodeToString(result);
		persist_plan(parse->queryId, list_length(hash_entry->plans_list), query_string, serialized_plan);

		last_query_id = parse->queryId;
	}
	else
	{
		if (enable_contrib && !hash_entry->plans_list)
			result = get_standart_plan(parse, query_string, cursorOptions, boundParams, es);
		else
			result = (PlannedStmt *) list_nth(hash_entry->plans_list, hash_entry->selected - 1);
	}

	MemoryContextSwitchTo(oldctx);

	return result;
}

void
_PG_init(void)
{
	HASHCTL ctl;

	ensure_memory_ctx();
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(uint64);
	ctl.entrysize = sizeof(PlanHashEntry);

	hashed_plans = hash_create("pg_multiplan hash table", 128, &ctl, HASH_ELEM | HASH_BLOBS);

	prev_planner_hook = planner_hook;
	planner_hook = multiplan_planner_hook;

	DefineCustomBoolVariable("pg_multiplan.enable_caching",
							"Enables plan caching.",
							NULL,
							&enable_caching,
							false,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	
	DefineCustomBoolVariable("pg_multiplan.enable",
							"Enables pg_multiplan.",
							NULL,
							&enable_contrib,
							false,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	
	MarkGUCPrefixReserved("pg_multiplan");
}

void
_PG_fini(void)
{
	planner_hook = prev_planner_hook;
	plan_cache_destroy();
}