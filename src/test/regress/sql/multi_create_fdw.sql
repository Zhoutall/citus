
SET citus.next_shard_id TO 390000;


-- ===================================================================
-- get ready for the foreign data wrapper tests
-- ===================================================================

-- create fake fdw for use in tests
CREATE FUNCTION fake_fdw_handler()
RETURNS fdw_handler
AS 'citus'
LANGUAGE C STRICT;

set citus.enable_ddl_propagation to off;
CREATE FOREIGN DATA WRAPPER fake_fdw HANDLER fake_fdw_handler;
CREATE SERVER fake_fdw_server FOREIGN DATA WRAPPER fake_fdw;
set citus.enable_ddl_propagation to on;

-- test propagating foreign server creation
CREATE EXTENSION postgres_fdw;
CREATE SERVER foreign_server TYPE 'test_type' VERSION 'v1'
        FOREIGN DATA WRAPPER postgres_fdw
        OPTIONS (host 'testhost', port '5432', dbname 'testdb');

SELECT COUNT(*)=1 FROM pg_foreign_server WHERE srvname = 'foreign_server';
\c - - - :worker_1_port
-- verify that the server is created on the worker
SELECT COUNT(*)=1 FROM pg_foreign_server WHERE srvname = 'foreign_server';
\c - - - :master_port
ALTER SERVER foreign_server OPTIONS (ADD passfile 'to_be_dropped');
ALTER SERVER foreign_server OPTIONS (SET host 'localhost');
ALTER SERVER foreign_server OPTIONS (DROP port, DROP dbname);
ALTER SERVER foreign_server OPTIONS (ADD port :'master_port', dbname 'regression', DROP passfile);
ALTER SERVER foreign_server RENAME TO foreign_server_1;

-- test alter owner
SELECT srvowner FROM pg_foreign_server WHERE srvname = 'foreign_server_1';
ALTER SERVER foreign_server_1 OWNER TO pg_monitor;
\c - - - :worker_1_port
-- verify that the server is renamed on the worker
SELECT srvoptions FROM pg_foreign_server WHERE srvname = 'foreign_server_1';
-- verify the owner is changed
SELECT srvowner FROM pg_foreign_server WHERE srvname = 'foreign_server_1';
ALTER SERVER foreign_server_1 OWNER TO postgres;
\c - - - :master_port

-- verify the owner is changed on the worker
SELECT srvowner FROM pg_foreign_server WHERE srvname = 'foreign_server_1';
DROP SERVER IF EXISTS foreign_server_1 CASCADE;
\c - - - :worker_1_port
-- verify that the server is dropped on the worker
SELECT COUNT(*)=0 FROM pg_foreign_server WHERE srvname = 'foreign_server_1';
\c - - - :master_port
