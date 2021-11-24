
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
CREATE SERVER foreign_server
        FOREIGN DATA WRAPPER postgres_fdw
        OPTIONS (host 'localhost', port :'master_port', dbname 'regression');

SELECT COUNT(*)=1 FROM pg_foreign_server WHERE srvname = 'foreign_server';
\c - - - :worker_1_port
-- verify that the server is created on the worker
SELECT COUNT(*)=1 FROM pg_foreign_server WHERE srvname = 'foreign_server';
\c - - - :master_port
ALTER SERVER foreign_server RENAME TO foreign_server_1;
\c - - - :worker_1_port
-- verify that the server is renamed on the worker
SELECT COUNT(*)=1 FROM pg_foreign_server WHERE srvname = 'foreign_server_1';
\c - - - :master_port

DROP SERVER foreign_server_1;
\c - - - :worker_1_port
-- verify that the server is dropped on the worker
SELECT COUNT(*)=0 FROM pg_foreign_server WHERE srvname = 'foreign_server_1';
\c - - - :master_port
