-- remove node to add later
SELECT citus_remove_node('localhost', :worker_1_port);

-- create schema, extension and foreign server while the worker is removed
CREATE SCHEMA test_dependent_schema;
CREATE EXTENSION postgres_fdw WITH SCHEMA test_dependent_schema;
CREATE SERVER foreign_server_dependent_schema
        FOREIGN DATA WRAPPER postgres_fdw
        OPTIONS (host 'test');

SELECT 1 FROM citus_add_node('localhost', :worker_1_port);

\c - - - :worker_1_port
-- verify the dependent schema and the foreign server are created on the newly added worker
SELECT COUNT(*) FROM pg_namespace WHERE nspname = 'test_dependent_schema';
SELECT COUNT(*)=1 FROM pg_foreign_server WHERE srvname = 'foreign_server_dependent_schema';
\c - - - :master_port
SET client_min_messages TO ERROR;
DROP SCHEMA test_dependent_schema CASCADE;
RESET client_min_messages;

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
ALTER SERVER foreign_server RENAME TO "foreign'server_1!";

-- test alter owner
SELECT srvowner FROM pg_foreign_server WHERE srvname = 'foreign''server_1!';
ALTER SERVER "foreign'server_1!" OWNER TO pg_monitor;
\c - - - :worker_1_port
-- verify that the server is renamed on the worker
SELECT srvoptions FROM pg_foreign_server WHERE srvname = 'foreign''server_1!';
-- verify the owner is changed
SELECT srvowner FROM pg_foreign_server WHERE srvname = 'foreign''server_1!';
-- this doesn't error out for now, since we don't have object metadata on the worker
ALTER SERVER "foreign'server_1!" OWNER TO postgres;
\c - - - :master_port

-- verify the owner is changed on the coordinator
SELECT srvowner FROM pg_foreign_server WHERE srvname = 'foreign''server_1!';
DROP SERVER IF EXISTS "foreign'server_1!" CASCADE;
\c - - - :worker_1_port
-- verify that the server is dropped on the worker
SELECT COUNT(*)=0 FROM pg_foreign_server WHERE srvname = 'foreign''server_1!';
\c - - - :master_port

