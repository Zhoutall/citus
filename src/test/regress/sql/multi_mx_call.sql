-- Test passing off CALL to mx workers

create schema multi_mx_call;
set search_path to multi_mx_call, public;

-- Create worker-local tables to test procedure calls were routed

set citus.shard_replication_factor to 2;

-- This table requires specific settings, create before getting into things
create table mx_call_dist_table_replica(id int, val int);
select create_distributed_table('mx_call_dist_table_replica', 'id');
insert into mx_call_dist_table_replica values (9,1),(8,2),(7,3),(6,4),(5,5);

set citus.shard_replication_factor to 1;

--
-- Create tables and procedures we want to use in tests
--
create table mx_call_dist_table_1(id int, val int);
select create_distributed_table('mx_call_dist_table_1', 'id');
insert into mx_call_dist_table_1 values (3,1),(4,5),(9,2),(6,5),(3,5);

create table mx_call_dist_table_2(id int, val int);
select create_distributed_table('mx_call_dist_table_2', 'id');
insert into mx_call_dist_table_2 values (1,1),(1,2),(2,2),(3,3),(3,4);

create table mx_call_dist_table_bigint(id bigint, val bigint);
select create_distributed_table('mx_call_dist_table_bigint', 'id');
insert into mx_call_dist_table_bigint values (1,1),(1,2),(2,2),(3,3),(3,4);

create table mx_call_dist_table_ref(id int, val int);
select create_reference_table('mx_call_dist_table_ref');
insert into mx_call_dist_table_ref values (2,7),(1,8),(2,8),(1,8),(2,8);

create type mx_call_enum as enum ('A', 'S', 'D', 'F');
create table mx_call_dist_table_enum(id int, key mx_call_enum);
select create_distributed_table('mx_call_dist_table_enum', 'key');
insert into mx_call_dist_table_enum values (1,'S'),(2,'A'),(3,'D'),(4,'F');

-- test that a distributed function can be colocated with a reference table
CREATE TABLE ref(groupid int);
SELECT create_reference_table('ref');

CREATE OR REPLACE PROCEDURE my_group_id_proc()
LANGUAGE plpgsql
SET search_path FROM CURRENT
AS $$
DECLARE
    gid int;
BEGIN
    SELECT groupid INTO gid
    FROM pg_dist_local_group;

    INSERT INTO ref(groupid) VALUES (gid);
END;
$$;

SELECT create_distributed_function('my_group_id_proc()', colocate_with := 'ref');

CALL my_group_id_proc();
CALL my_group_id_proc();
SELECT DISTINCT(groupid) FROM ref ORDER BY 1;
TRUNCATE TABLE ref;

-- test round robin task assignment policy uses different workers on consecutive procedure calls.
SET citus.task_assignment_policy TO 'round-robin';
CALL my_group_id_proc();
CALL my_group_id_proc();
CALL my_group_id_proc();
SELECT DISTINCT(groupid) FROM ref ORDER BY 1;
TRUNCATE TABLE ref;

RESET citus.task_assignment_policy;

CREATE PROCEDURE mx_call_proc(x int, INOUT y int)
LANGUAGE plpgsql AS $$
BEGIN
    -- groupid is 0 in coordinator and non-zero in workers, so by using it here
    -- we make sure the procedure is being executed in the worker.
    y := x + (select case groupid when 0 then 1 else 0 end from pg_dist_local_group);
    -- we also make sure that we can run distributed queries in the procedures
    -- that are routed to the workers.
    y := y + (select sum(t1.val + t2.val) from multi_mx_call.mx_call_dist_table_1 t1 join multi_mx_call.mx_call_dist_table_2 t2 on t1.id = t2.id);
END;$$;

CREATE PROCEDURE mx_call_proc_bigint(x bigint, INOUT y bigint)
LANGUAGE plpgsql AS $$
BEGIN
    y := x + y * 2;
END;$$;

-- create another procedure which verifies:
-- 1. we work fine with multiple return columns
-- 2. we work fine in combination with custom types
CREATE PROCEDURE mx_call_proc_custom_types(INOUT x mx_call_enum, INOUT y mx_call_enum)
LANGUAGE plpgsql AS $$
BEGIN
    y := x;
    x := (select case groupid when 0 then 'F' else 'S' end from pg_dist_local_group);
END;$$;

-- Test that undistributed procedures have no issue executing
call multi_mx_call.mx_call_proc(2, 0);
call multi_mx_call.mx_call_proc_custom_types('S', 'A');

-- Same for unqualified names
call mx_call_proc(2, 0);
call mx_call_proc_custom_types('S', 'A');

-- Mark both procedures as distributed ...
select create_distributed_function('mx_call_proc(int,int)');
select create_distributed_function('mx_call_proc_bigint(bigint,bigint)');
select create_distributed_function('mx_call_proc_custom_types(mx_call_enum,mx_call_enum)');

-- We still don't route them to the workers, because they aren't
-- colocated with any distributed tables.
SET client_min_messages TO DEBUG1;
call multi_mx_call.mx_call_proc(2, 0);
call mx_call_proc_bigint(4, 2);
call multi_mx_call.mx_call_proc_custom_types('S', 'A');

-- Mark them as colocated with a table. Now we should route them to workers.
select colocate_proc_with_table('mx_call_proc', 'mx_call_dist_table_1'::regclass, 1);
select colocate_proc_with_table('mx_call_proc_bigint', 'mx_call_dist_table_bigint'::regclass, 1);
select colocate_proc_with_table('mx_call_proc_custom_types', 'mx_call_dist_table_enum'::regclass, 1);

call multi_mx_call.mx_call_proc(2, 0);
call multi_mx_call.mx_call_proc_custom_types('S', 'A');
call mx_call_proc(2, 0);
call mx_call_proc_custom_types('S', 'A');

-- Test implicit cast of int to bigint
call mx_call_proc_bigint(4, 2);

-- We don't allow distributing calls inside transactions
begin;
call multi_mx_call.mx_call_proc(2, 0);
commit;

-- Drop the table colocated with mx_call_proc_custom_types. Now it shouldn't
-- be routed to workers anymore.
SET client_min_messages TO NOTICE;
drop table mx_call_dist_table_enum;
SET client_min_messages TO DEBUG1;
call multi_mx_call.mx_call_proc_custom_types('S', 'A');

-- Make sure we do bounds checking on distributed argument index
-- This also tests that we have cache invalidation for pg_dist_object updates
select colocate_proc_with_table('mx_call_proc', 'mx_call_dist_table_1'::regclass, -1);
call multi_mx_call.mx_call_proc(2, 0);
select colocate_proc_with_table('mx_call_proc', 'mx_call_dist_table_1'::regclass, 2);
call multi_mx_call.mx_call_proc(2, 0);

-- We support colocating with reference tables
select colocate_proc_with_table('mx_call_proc', 'mx_call_dist_table_ref'::regclass, NULL);
call multi_mx_call.mx_call_proc(2, 0);

-- We don't currently support colocating with replicated tables
select colocate_proc_with_table('mx_call_proc', 'mx_call_dist_table_replica'::regclass, 1);
call multi_mx_call.mx_call_proc(2, 0);
SET client_min_messages TO NOTICE;
drop table mx_call_dist_table_replica;
SET client_min_messages TO DEBUG1;

select colocate_proc_with_table('mx_call_proc', 'mx_call_dist_table_1'::regclass, 1);

-- Test that we handle transactional constructs correctly inside a procedure
-- that is routed to the workers.
CREATE PROCEDURE mx_call_proc_tx(x int) LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO multi_mx_call.mx_call_dist_table_1 VALUES (x, -1), (x+1, 4);
    COMMIT;
    UPDATE multi_mx_call.mx_call_dist_table_1 SET val = val+1 WHERE id >= x;
    ROLLBACK;
    -- Now do the final update!
    UPDATE multi_mx_call.mx_call_dist_table_1 SET val = val-1 WHERE id >= x;
END;$$;

-- before distribution ...
CALL multi_mx_call.mx_call_proc_tx(10);
-- after distribution ...
select create_distributed_function('mx_call_proc_tx(int)', '$1', 'mx_call_dist_table_1');
CALL multi_mx_call.mx_call_proc_tx(20);
SELECT id, val FROM mx_call_dist_table_1 ORDER BY id, val;

-- Show that function delegation works from worker nodes as well
\c - - - :worker_1_port
SET search_path to multi_mx_call, public;
SET client_min_messages TO DEBUG1;
CALL multi_mx_call.mx_call_proc_tx(9);

\c - - - :master_port
SET search_path to multi_mx_call, public;
SET client_min_messages TO DEBUG1;

-- Test that we properly propagate errors raised from procedures.
CREATE PROCEDURE mx_call_proc_raise(x int) LANGUAGE plpgsql AS $$
BEGIN
    RAISE WARNING 'warning';
    RAISE EXCEPTION 'error';
END;$$;
select create_distributed_function('mx_call_proc_raise(int)', '$1', 'mx_call_dist_table_1');
\set VERBOSITY terse
call multi_mx_call.mx_call_proc_raise(2);
\set VERBOSITY default

-- Test that we don't propagate to non-metadata worker nodes
SET client_min_messages TO WARNING;
select stop_metadata_sync_to_node('localhost', :worker_1_port);
select stop_metadata_sync_to_node('localhost', :worker_2_port);
SET client_min_messages TO DEBUG1;
call multi_mx_call.mx_call_proc(2, 0);
SET client_min_messages TO NOTICE;
select start_metadata_sync_to_node('localhost', :worker_1_port);
select start_metadata_sync_to_node('localhost', :worker_2_port);

-- stop_metadata_sync_to_node()/start_metadata_sync_to_node() might make
-- worker backend caches inconsistent. Reconnect to coordinator to use
-- new worker connections, hence new backends.
\c - - - :master_port
SET search_path to multi_mx_call, public;
SET client_min_messages TO DEBUG1;

--
-- Test non-const parameter values
--
CREATE FUNCTION mx_call_add(int, int) RETURNS int
    AS 'select $1 + $2;' LANGUAGE SQL IMMUTABLE;
SELECT create_distributed_function('mx_call_add(int,int)');

-- non-const distribution parameters cannot be pushed down
call multi_mx_call.mx_call_proc(2, mx_call_add(3, 4));

-- non-const parameter can be pushed down
call multi_mx_call.mx_call_proc(multi_mx_call.mx_call_add(3, 4), 2);

-- volatile parameter cannot be pushed down
call multi_mx_call.mx_call_proc(floor(random())::int, 2);

reset client_min_messages;
\set VERBOSITY terse
drop schema multi_mx_call cascade;

