\echo Use "CREATE EXTENSION pg_multiplan" to load this file. \quit

CREATE TABLE pg_multiplan_plans (
    query_id    text NOT NULL,
    plan_id     bigint NOT NULL,
    query_text  text,
    plan_data   text NOT NULL,
    selected    boolean DEFAULT false
);

CREATE FUNCTION pg_multiplan_list(
	OUT plan_id		bigint,
    OUT query_id	text,
    OUT total_cost	double precision,
    OUT query_text	text,
	OUT selected	boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_multiplan_list'
LANGUAGE C STRICT;

CREATE FUNCTION pg_multiplan_list(
	IN  filter_query_id	bigint,
	OUT plan_id			bigint,
	OUT query_id		text,
	OUT total_cost		double precision,
	OUT query_text		text,
	OUT selected		boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_multiplan_list'
LANGUAGE C STRICT;

CREATE FUNCTION pg_multiplan_select(
	IN query_id	text,
	IN plan_id	bigint
)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_multiplan_select'
LANGUAGE C STRICT;
