-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgaudit" to load this file.\quit

CREATE FUNCTION pgaudit_ddl_command_end()
	RETURNS event_trigger
	SECURITY DEFINER
	SET search_path = 'pg_catalog, pg_temp'
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'pgaudit_ddl_command_end';

CREATE EVENT TRIGGER pgaudit_ddl_command_end
	ON ddl_command_end
	EXECUTE PROCEDURE pgaudit_ddl_command_end();

CREATE FUNCTION pgaudit_sql_drop()
	RETURNS event_trigger
	SECURITY DEFINER
	SET search_path = 'pg_catalog, pg_temp'
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'pgaudit_sql_drop';

CREATE EVENT TRIGGER pgaudit_sql_drop
	ON sql_drop
	EXECUTE PROCEDURE pgaudit_sql_drop();


CREATE OR REPLACE FUNCTION
pg_catalog.gp_event_trigger_ddl_commands(
	OUT classid oid,
	OUT objid oid,
	OUT objsubid integer,
	OUT command_tag text,
	OUT object_type text,
	OUT schema_name text,
	OUT object_identity text,
	OUT in_extension boolean
)
AS 'MODULE_PATHNAME'
VOLATILE
EXECUTE ON MASTER
LANGUAGE C STRICT;
