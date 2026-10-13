#include "postgres.h"
#include "access/table.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/extension.h"
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
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/guc.h"

#include <stdbool.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_multiplan_list);
PG_FUNCTION_INFO_V1(pg_multiplan_select);

enum
{
	Anum_pg_multiplan_plans_query_id = 1,
	Anum_pg_multiplan_plans_query_tree,
	Anum_pg_multiplan_plans_plan_id,
	Anum_pg_multiplan_plans_query_text,
	Anum_pg_multiplan_plans_plan_data,
	Anum_pg_multiplan_plans_selected,

	Natts_pg_multiplan_plans = Anum_pg_multiplan_plans_selected	/* number of columns */
};

static MemoryContext plan_cache_ctx = NULL;
static planner_hook_type prev_planner_hook = NULL;
static HTAB *hashed_plans = NULL;
static uint64 last_query_id = 0;
static bool cache_loaded = false;

// guc tables
static bool enable_caching = false;
static bool enable_contrib = false;

typedef struct CachedPlanEntry
{
	PlannedStmt	*stmt;
	Query		*query;
	char		*query_text;
} CachedPlanEntry;

typedef struct PlanHashEntry
{
	uint64 queryId;
	List *plans_list;
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

static Oid
get_plans_relid(void)
{
	Oid extension_relid;
	Oid schema_relid;
	Oid	relname_relid;

	extension_relid = get_extension_oid("pg_multiplan", true);

	if (!OidIsValid(extension_relid))
		return InvalidOid;
	
	schema_relid = get_extension_schema(extension_relid);

	if (!OidIsValid(schema_relid))
		return InvalidOid;

	relname_relid = get_relname_relid("pg_multiplan_plans", schema_relid);

	if (!OidIsValid(relname_relid)
		|| getExtensionOfObject(RelationRelationId, relname_relid) != extension_relid)
		return InvalidOid;
	
	return relname_relid;
}

static void
load_plans_from_table(void)
{
	Oid				relid;
	Relation		rel;
	TableScanDesc	scan;
	HeapTuple		tup;
	TupleDesc		tupdesc;
	MemoryContext	oldctx;
	bool			snapshot_set = false;

	relid = get_plans_relid();

	if (!OidIsValid(relid))
		return;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	rel = table_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, SO_NONE);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Datum   d_qid, d_qtext, d_pdata, d_qtree;
		// Datum d_plan_id;
		bool    isnull;
		uint64  query_id;
		char	*plan_str, *query_str, *query_tree_str;
		PlannedStmt *plan;
		PlanHashEntry *entry;
		bool    found;
		CachedPlanEntry *cached_plan;

		d_qid   = heap_getattr(tup, Anum_pg_multiplan_plans_query_id, tupdesc, &isnull);

		if (isnull)
		{
			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("skipping plan with null \"%s\" in table \"pg_multiplan_plans\"",
							"query_id")));
			continue;
		}

		d_qtree = heap_getattr(tup, Anum_pg_multiplan_plans_query_tree, tupdesc, &isnull);

		if (isnull)
		{
			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("skipping plan with null \"%s\" in table \"pg_multiplan_plans\"",
							"query_tree")));
			continue;
		}

		// d_plan_id = heap_getattr(tup, Anum_pg_multiplan_plans_plan_id, tupdesc, &isnull);
		d_pdata = heap_getattr(tup, Anum_pg_multiplan_plans_plan_data, tupdesc, &isnull);

		if (isnull)
		{
			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("skipping plan with null \"%s\" in table \"pg_multiplan_plans\"",
							"plan_data")));
			continue;
		}

		d_qtext = heap_getattr(tup, Anum_pg_multiplan_plans_query_text, tupdesc, &isnull);

		query_id = strtoull(text_to_cstring(DatumGetTextPP(d_qid)), NULL, 10);
		plan_str = text_to_cstring(DatumGetTextPP(d_pdata));
		query_tree_str = text_to_cstring(DatumGetTextPP(d_qtree));
		query_str = isnull ? NULL : text_to_cstring(DatumGetTextPP(d_qtext));

		oldctx = MemoryContextSwitchTo(plan_cache_ctx);

		plan = (PlannedStmt *) stringToNode(plan_str);

		cached_plan = palloc(sizeof(CachedPlanEntry));
		cached_plan->query = (Query *) stringToNode(query_tree_str);
		cached_plan->query_text = query_str ? pstrdup(query_str) : NULL;
		cached_plan->stmt = plan;

		entry = hash_search(hashed_plans, &query_id, HASH_ENTER, &found);
		if (!found)
		{
			entry->plans_list = NIL;
			entry->selected = 1;
		}

		entry->plans_list = lappend(entry->plans_list, cached_plan);

		MemoryContextSwitchTo(oldctx);
	}

	table_endscan(scan);

	if (snapshot_set)
		PopActiveSnapshot();

	table_close(rel, AccessShareLock);
}

static void
persist_plan(uint64 query_id, const char *query_tree, int64 plan_id,
			 const char *query_text, const char *plan_data)
{
	Oid         relid;
	Relation    rel;
	Datum       values[Natts_pg_multiplan_plans];
	bool        nulls[Natts_pg_multiplan_plans] = {0};
	HeapTuple   tup;
	char        qid_buf[32];

	snprintf(qid_buf, sizeof(qid_buf), UINT64_FORMAT, query_id);

	relid = get_plans_relid();

	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("extension \"pg_multiplan\" is not installed in this database"),
				 errhint("Run CREATE EXTENSION pg_multiplan, or turn off pg_multiplan.enable_caching.")));

	rel = table_open(relid, RowExclusiveLock);

	values[Anum_pg_multiplan_plans_query_id - 1] = CStringGetTextDatum(qid_buf);
	values[Anum_pg_multiplan_plans_query_tree - 1] = CStringGetTextDatum(query_tree);
	values[Anum_pg_multiplan_plans_plan_id - 1] = Int64GetDatum(plan_id);
	if (query_text)
		values[Anum_pg_multiplan_plans_query_text - 1] = CStringGetTextDatum(query_text);
	else
		nulls[Anum_pg_multiplan_plans_query_text - 1] = true;
	values[Anum_pg_multiplan_plans_plan_data - 1] = CStringGetTextDatum(plan_data);
	values[Anum_pg_multiplan_plans_selected - 1] = BoolGetDatum(false);

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
		CachedPlanEntry *entry = (CachedPlanEntry *) lfirst(lc);
		Datum	values[5];
		bool	nulls[5] = {0};

		bool	is_selected = (hash_entry->selected == plan_id);
		char	qid_buf[32];
		snprintf(qid_buf, sizeof(qid_buf), UINT64_FORMAT, (uint64) hash_entry->queryId);

		values[0] = Int64GetDatum(plan_id++);
		values[1] = CStringGetTextDatum(qid_buf);
		values[2] = Float8GetDatum(entry->stmt->planTree->total_cost);
		if (entry->query_text)
			values[3] = CStringGetTextDatum(entry->query_text);
		else
			nulls[3] = true;
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
	Query		*query_copy;
	CachedPlanEntry *cached_plan;

	if (!enable_contrib || boundParams != NULL)
		return get_standart_plan(parse, query_string, cursorOptions,
								 boundParams, es);

	if (!cache_loaded)
	{
		load_plans_from_table();
		cache_loaded = true;
	}

	if (parse->queryId == INT64CONST(0))
		JumbleQuery(parse);

	query_copy = copyObject(parse);

	if (!enable_caching)
	{
		hash_entry = (PlanHashEntry *) hash_search(hashed_plans, &parse->queryId,
												   HASH_FIND, &hash_found);

		if (hash_found &&
			hash_entry->selected >= 1 &&
			hash_entry->selected <= list_length(hash_entry->plans_list))
		{
			cached_plan =
				list_nth(hash_entry->plans_list, hash_entry->selected - 1);

			if (equal(cached_plan->query, parse))
				return copyObject(cached_plan->stmt);
		}

		return get_standart_plan(parse, query_string, cursorOptions,
								 boundParams, es);
	}

	result = get_standart_plan(parse, query_string, cursorOptions,
							   boundParams, es);

	oldctx = MemoryContextSwitchTo(plan_cache_ctx);

	hash_entry = (PlanHashEntry *) hash_search(hashed_plans, &parse->queryId,
											   HASH_ENTER, &hash_found);
	if (!hash_found)
	{
		hash_entry->plans_list = NIL;
		hash_entry->selected = 1;
	}

	cached_plan = palloc(sizeof(CachedPlanEntry));
	cached_plan->stmt = copyObject(result);
	cached_plan->query = copyObject(query_copy);
	cached_plan->query_text = query_string ? pstrdup(query_string) : NULL;
	hash_entry->plans_list = lappend(hash_entry->plans_list, cached_plan);

	MemoryContextSwitchTo(oldctx);

	persist_plan(parse->queryId, nodeToString(query_copy), list_length(hash_entry->plans_list),
				 query_string, nodeToString(result));

	last_query_id = parse->queryId;

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