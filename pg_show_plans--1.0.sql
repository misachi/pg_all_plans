/* contrib/pg_show_plans/pg_show_plans--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_show_plans" to load this file. \quit

--
-- show_all_plans()
--

CREATE FUNCTION show_all_plans(IN query_string text, OUT query_plans text)
RETURNS setof text
AS 'MODULE_PATHNAME', 'show_all_plans'
LANGUAGE C VOLATILE;

REVOKE EXECUTE ON FUNCTION show_all_plans(TEXT) FROM PUBLIC;
