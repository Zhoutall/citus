/*-------------------------------------------------------------------------
 *
 * metadata_utility.c
 *    Routines for reading and modifying master node's metadata.
 *
 * Copyright (c) Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include <sys/statvfs.h>

#include "postgres.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "miscadmin.h"

#include "distributed/pg_version_constants.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "distributed/colocation_utils.h"
#include "distributed/connection_management.h"
#include "distributed/citus_nodes.h"
#include "distributed/citus_safe_lib.h"
#include "distributed/listutils.h"
#include "distributed/lock_graph.h"
#include "distributed/metadata_utility.h"
#include "distributed/coordinator_protocol.h"
#include "distributed/metadata_cache.h"
#include "distributed/multi_join_order.h"
#include "distributed/multi_logical_optimizer.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/pg_dist_colocation.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/pg_dist_shard.h"
#include "distributed/pg_dist_placement.h"
#include "distributed/reference_table_utils.h"
#include "distributed/relay_utility.h"
#include "distributed/resource_lock.h"
#include "distributed/remote_commands.h"
#include "distributed/tuplestore.h"
#include "distributed/worker_manager.h"
#include "distributed/worker_protocol.h"
#include "distributed/version_compat.h"
#include "nodes/makefuncs.h"
#include "parser/scansup.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#define DISK_SPACE_FIELDS 2

/* Local functions forward declarations */
static uint64 * AllocateUint64(uint64 value);
static void RecordDistributedRelationDependencies(Oid distributedRelationId);
static GroupShardPlacement * TupleToGroupShardPlacement(TupleDesc tupleDesc,
														HeapTuple heapTuple);
static bool DistributedTableSize(Oid relationId, SizeQueryType sizeQueryType,
								 bool failOnError, uint64 *tableSize);
static bool DistributedTableSizeOnWorker(WorkerNode *workerNode, Oid relationId,
										 SizeQueryType sizeQueryType, bool failOnError,
										 uint64 *tableSize);
static List * ShardIntervalsOnWorkerGroup(WorkerNode *workerNode, Oid relationId);
static char * GenerateShardStatisticsQueryForShardList(List *shardIntervalList);
static char * GetWorkerPartitionedSizeUDFNameBySizeQueryType(SizeQueryType sizeQueryType);
static char * GetSizeQueryBySizeQueryType(SizeQueryType sizeQueryType);
static char * GenerateAllShardStatisticsQueryForNode(WorkerNode *workerNode,
													 List *citusTableIds);
static List * GenerateShardStatisticsQueryList(List *workerNodeList, List *citusTableIds);
static void ErrorIfNotSuitableToGetSize(Oid relationId);
static List * OpenConnectionToNodes(List *workerNodeList);
static void ReceiveShardNameAndSizeResults(List *connectionList,
										   Tuplestorestate *tupleStore,
										   TupleDesc tupleDescriptor);
static void AppendShardSizeQuery(StringInfo selectQuery, ShardInterval *shardInterval);

static HeapTuple CreateDiskSpaceTuple(TupleDesc tupleDesc, uint64 availableBytes,
									  uint64 totalBytes);
static bool GetLocalDiskSpaceStats(uint64 *availableBytes, uint64 *totalBytes);

/* exports for SQL callable functions */
PG_FUNCTION_INFO_V1(citus_local_disk_space_stats);
PG_FUNCTION_INFO_V1(citus_table_size);
PG_FUNCTION_INFO_V1(citus_total_relation_size);
PG_FUNCTION_INFO_V1(citus_relation_size);
PG_FUNCTION_INFO_V1(citus_shard_sizes);


/*
 * CreateDiskSpaceTuple creates a tuple that is used as the return value
 * for citus_local_disk_space_stats.
 */
static HeapTuple
CreateDiskSpaceTuple(TupleDesc tupleDescriptor, uint64 availableBytes, uint64 totalBytes)
{
	Datum values[DISK_SPACE_FIELDS];
	bool isNulls[DISK_SPACE_FIELDS];

	/* form heap tuple for remote disk space statistics */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[0] = UInt64GetDatum(availableBytes);
	values[1] = UInt64GetDatum(totalBytes);

	HeapTuple diskSpaceTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	return diskSpaceTuple;
}


/*
 * citus_local_disk_space_stats returns total disk space and available disk
 * space for the disk that contains PGDATA.
 */
Datum
citus_local_disk_space_stats(PG_FUNCTION_ARGS)
{
	uint64 availableBytes = 0;
	uint64 totalBytes = 0;

	if (!GetLocalDiskSpaceStats(&availableBytes, &totalBytes))
	{
		ereport(WARNING, (errmsg("could not get disk space")));
	}

	TupleDesc tupleDescriptor = NULL;

	TypeFuncClass resultTypeClass = get_call_result_type(fcinfo, NULL,
														 &tupleDescriptor);
	if (resultTypeClass != TYPEFUNC_COMPOSITE)
	{
		ereport(ERROR, (errmsg("return type must be a row type")));
	}

	HeapTuple diskSpaceTuple = CreateDiskSpaceTuple(tupleDescriptor, availableBytes,
													totalBytes);

	PG_RETURN_DATUM(HeapTupleGetDatum(diskSpaceTuple));
}


/*
 * GetLocalDiskSpaceStats returns total and available disk space for the disk containing
 * PGDATA (not considering tablespaces, quota).
 */
static bool
GetLocalDiskSpaceStats(uint64 *availableBytes, uint64 *totalBytes)
{
	struct statvfs buffer;
	if (statvfs(DataDir, &buffer) != 0)
	{
		return false;
	}

	/*
	 * f_bfree: number of free blocks
	 * f_frsize: fragment size, same as f_bsize usually
	 * f_blocks: Size of fs in f_frsize units
	 */
	*availableBytes = buffer.f_bfree * buffer.f_frsize;
	*totalBytes = buffer.f_blocks * buffer.f_frsize;

	return true;
}


/*
 * GetNodeDiskSpaceStatsForConnection fetches the disk space statistics for the node
 * that is on the given connection, or returns false if unsuccessful.
 */
bool
GetNodeDiskSpaceStatsForConnection(MultiConnection *connection, uint64 *availableBytes,
								   uint64 *totalBytes)
{
	PGresult *result = NULL;

	char *sizeQuery = "SELECT available_disk_size, total_disk_size "
					  "FROM pg_catalog.citus_local_disk_space_stats()";


	int queryResult = ExecuteOptionalRemoteCommand(connection, sizeQuery, &result);
	if (queryResult != RESPONSE_OKAY || !IsResponseOK(result) || PQntuples(result) != 1)
	{
		ereport(WARNING, (errcode(ERRCODE_CONNECTION_FAILURE),
						  errmsg("cannot get the disk space statistics for node %s:%d",
								 connection->hostname, connection->port)));

		PQclear(result);
		ForgetResults(connection);

		return false;
	}

	char *availableBytesString = PQgetvalue(result, 0, 0);
	char *totalBytesString = PQgetvalue(result, 0, 1);

	*availableBytes = SafeStringToUint64(availableBytesString);
	*totalBytes = SafeStringToUint64(totalBytesString);

	PQclear(result);
	ForgetResults(connection);

	return true;
}


/*
 * citus_shard_sizes returns all shard names and their sizes.
 */
Datum
citus_shard_sizes(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	List *allCitusTableIds = AllCitusTableIds();

	/* we don't need a distributed transaction here */
	bool useDistributedTransaction = false;

	List *connectionList =
		SendShardStatisticsQueriesInParallel(allCitusTableIds, useDistributedTransaction);

	TupleDesc tupleDescriptor = NULL;
	Tuplestorestate *tupleStore = SetupTuplestore(fcinfo, &tupleDescriptor);

	ReceiveShardNameAndSizeResults(connectionList, tupleStore, tupleDescriptor);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupleStore);

	PG_RETURN_VOID();
}


/*
 * citus_total_relation_size accepts a table name and returns a distributed table
 * and its indexes' total relation size.
 */
Datum
citus_total_relation_size(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	Oid relationId = PG_GETARG_OID(0);
	bool failOnError = PG_GETARG_BOOL(1);

	SizeQueryType sizeQueryType = TOTAL_RELATION_SIZE;
	uint64 tableSize = 0;

	if (!DistributedTableSize(relationId, sizeQueryType, failOnError, &tableSize))
	{
		Assert(!failOnError);
		PG_RETURN_NULL();
	}

	PG_RETURN_INT64(tableSize);
}


/*
 * citus_table_size accepts a table name and returns a distributed table's total
 * relation size.
 */
Datum
citus_table_size(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	Oid relationId = PG_GETARG_OID(0);
	bool failOnError = true;
	SizeQueryType sizeQueryType = TABLE_SIZE;
	uint64 tableSize = 0;

	if (!DistributedTableSize(relationId, sizeQueryType, failOnError, &tableSize))
	{
		Assert(!failOnError);
		PG_RETURN_NULL();
	}

	PG_RETURN_INT64(tableSize);
}


/*
 * citus_relation_size accept a table name and returns a relation's 'main'
 * fork's size.
 */
Datum
citus_relation_size(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	Oid relationId = PG_GETARG_OID(0);
	bool failOnError = true;
	SizeQueryType sizeQueryType = RELATION_SIZE;
	uint64 relationSize = 0;

	if (!DistributedTableSize(relationId, sizeQueryType, failOnError, &relationSize))
	{
		Assert(!failOnError);
		PG_RETURN_NULL();
	}

	PG_RETURN_INT64(relationSize);
}


/*
 * SendShardStatisticsQueriesInParallel generates query lists for obtaining shard
 * statistics and then sends the commands in parallel by opening connections
 * to available nodes. It returns the connection list.
 */
List *
SendShardStatisticsQueriesInParallel(List *citusTableIds, bool useDistributedTransaction)
{
	List *workerNodeList = ActivePrimaryNodeList(NoLock);

	List *shardSizesQueryList = GenerateShardStatisticsQueryList(workerNodeList,
																 citusTableIds);

	List *connectionList = OpenConnectionToNodes(workerNodeList);
	FinishConnectionListEstablishment(connectionList);

	if (useDistributedTransaction)
	{
		/*
		 * For now, in the case we want to include shard min and max values, we also
		 * want to update the entries in pg_dist_placement and pg_dist_shard with the
		 * latest statistics. In order to detect distributed deadlocks, we assign a
		 * distributed transaction ID to the current transaction
		 */
		UseCoordinatedTransaction();
	}

	/* send commands in parallel */
	for (int i = 0; i < list_length(connectionList); i++)
	{
		MultiConnection *connection = (MultiConnection *) list_nth(connectionList, i);
		char *shardSizesQuery = (char *) list_nth(shardSizesQueryList, i);

		if (useDistributedTransaction)
		{
			/* run the size query in a distributed transaction */
			RemoteTransactionBeginIfNecessary(connection);
		}

		int querySent = SendRemoteCommand(connection, shardSizesQuery);

		if (querySent == 0)
		{
			ReportConnectionError(connection, WARNING);
		}
	}
	return connectionList;
}


/*
 * OpenConnectionToNodes opens a single connection per node
 * for the given workerNodeList.
 */
static List *
OpenConnectionToNodes(List *workerNodeList)
{
	List *connectionList = NIL;
	WorkerNode *workerNode = NULL;
	foreach_ptr(workerNode, workerNodeList)
	{
		const char *nodeName = workerNode->workerName;
		int nodePort = workerNode->workerPort;
		int connectionFlags = 0;

		MultiConnection *connection = StartNodeConnection(connectionFlags, nodeName,
														  nodePort);

		connectionList = lappend(connectionList, connection);
	}
	return connectionList;
}


/*
 * GenerateShardStatisticsQueryList generates a query per node that will return:
 * shard_id, shard_name, shard_size for all shard placements on the node
 */
static List *
GenerateShardStatisticsQueryList(List *workerNodeList, List *citusTableIds)
{
	List *shardStatisticsQueryList = NIL;
	WorkerNode *workerNode = NULL;
	foreach_ptr(workerNode, workerNodeList)
	{
		char *shardStatisticsQuery =
			GenerateAllShardStatisticsQueryForNode(workerNode, citusTableIds);

		shardStatisticsQueryList = lappend(shardStatisticsQueryList,
										   shardStatisticsQuery);
	}
	return shardStatisticsQueryList;
}


/*
 * ReceiveShardNameAndSizeResults receives shard name and size results from the given
 * connection list.
 */
static void
ReceiveShardNameAndSizeResults(List *connectionList, Tuplestorestate *tupleStore,
							   TupleDesc tupleDescriptor)
{
	MultiConnection *connection = NULL;
	foreach_ptr(connection, connectionList)
	{
		bool raiseInterrupts = true;
		Datum values[SHARD_SIZES_COLUMN_COUNT];
		bool isNulls[SHARD_SIZES_COLUMN_COUNT];

		if (PQstatus(connection->pgConn) != CONNECTION_OK)
		{
			continue;
		}

		PGresult *result = GetRemoteCommandResult(connection, raiseInterrupts);
		if (!IsResponseOK(result))
		{
			ReportResultError(connection, result, WARNING);
			continue;
		}

		int64 rowCount = PQntuples(result);
		int64 colCount = PQnfields(result);

		/* Although it is not expected */
		if (colCount != SHARD_SIZES_COLUMN_COUNT)
		{
			ereport(WARNING, (errmsg("unexpected number of columns from "
									 "citus_shard_sizes")));
			continue;
		}

		for (int64 rowIndex = 0; rowIndex < rowCount; rowIndex++)
		{
			memset(values, 0, sizeof(values));
			memset(isNulls, false, sizeof(isNulls));

			/* format is [0] shard id, [1] shard name, [2] size */
			char *tableName = PQgetvalue(result, rowIndex, 1);
			Datum resultStringDatum = CStringGetDatum(tableName);
			Datum textDatum = DirectFunctionCall1(textin, resultStringDatum);

			values[0] = textDatum;
			values[1] = ParseIntField(result, rowIndex, 2);

			tuplestore_putvalues(tupleStore, tupleDescriptor, values, isNulls);
		}

		PQclear(result);
		ForgetResults(connection);
	}
}


/*
 * DistributedTableSize is helper function for each kind of citus size functions.
 * It first checks whether the table is distributed and size query can be run on
 * it. Connection to each node has to be established to get the size of the table.
 */
static bool
DistributedTableSize(Oid relationId, SizeQueryType sizeQueryType, bool failOnError,
					 uint64 *tableSize)
{
	int logLevel = WARNING;

	if (failOnError)
	{
		logLevel = ERROR;
	}

	uint64 sumOfSizes = 0;

	if (XactModificationLevel == XACT_MODIFICATION_DATA)
	{
		ereport(logLevel, (errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
						   errmsg("citus size functions cannot be called in transaction "
								  "blocks which contain multi-shard data "
								  "modifications")));

		return false;
	}

	Relation relation = try_relation_open(relationId, AccessShareLock);

	if (relation == NULL)
	{
		ereport(logLevel,
				(errmsg("could not compute table size: relation does not exist")));

		return false;
	}

	ErrorIfNotSuitableToGetSize(relationId);

	table_close(relation, AccessShareLock);

	List *workerNodeList = ActiveReadableNodeList();
	WorkerNode *workerNode = NULL;
	foreach_ptr(workerNode, workerNodeList)
	{
		uint64 relationSizeOnNode = 0;

		bool gotSize = DistributedTableSizeOnWorker(workerNode, relationId, sizeQueryType,
													failOnError, &relationSizeOnNode);
		if (!gotSize)
		{
			return false;
		}

		sumOfSizes += relationSizeOnNode;
	}

	*tableSize = sumOfSizes;

	return true;
}


/*
 * DistributedTableSizeOnWorker gets the workerNode and relationId to calculate
 * size of that relation on the given workerNode by summing up the size of each
 * shard placement.
 */
static bool
DistributedTableSizeOnWorker(WorkerNode *workerNode, Oid relationId,
							 SizeQueryType sizeQueryType,
							 bool failOnError, uint64 *tableSize)
{
	int logLevel = WARNING;

	if (failOnError)
	{
		logLevel = ERROR;
	}

	char *workerNodeName = workerNode->workerName;
	uint32 workerNodePort = workerNode->workerPort;
	uint32 connectionFlag = 0;
	PGresult *result = NULL;

	List *shardIntervalsOnNode = ShardIntervalsOnWorkerGroup(workerNode, relationId);

	/*
	 * We pass false here, because if we optimize this, we would include child tables.
	 * But citus size functions shouldn't include them, like PG.
	 */
	bool optimizePartitionCalculations = false;
	StringInfo tableSizeQuery = GenerateSizeQueryOnMultiplePlacements(
		shardIntervalsOnNode,
		sizeQueryType,
		optimizePartitionCalculations);

	MultiConnection *connection = GetNodeConnection(connectionFlag, workerNodeName,
													workerNodePort);
	int queryResult = ExecuteOptionalRemoteCommand(connection, tableSizeQuery->data,
												   &result);

	if (queryResult != 0)
	{
		ereport(logLevel, (errcode(ERRCODE_CONNECTION_FAILURE),
						   errmsg("could not connect to %s:%d to get size of "
								  "table \"%s\"",
								  workerNodeName, workerNodePort,
								  get_rel_name(relationId))));

		return false;
	}

	List *sizeList = ReadFirstColumnAsText(result);
	if (list_length(sizeList) != 1)
	{
		PQclear(result);
		ClearResults(connection, failOnError);

		ereport(logLevel, (errcode(ERRCODE_CONNECTION_FAILURE),
						   errmsg("cannot parse size of table \"%s\" from %s:%d",
								  get_rel_name(relationId), workerNodeName,
								  workerNodePort)));

		return false;
	}

	StringInfo tableSizeStringInfo = (StringInfo) linitial(sizeList);
	char *tableSizeString = tableSizeStringInfo->data;

	if (strlen(tableSizeString) > 0)
	{
		*tableSize = SafeStringToUint64(tableSizeString);
	}
	else
	{
		/*
		 * This means the shard is moved or dropped while citus_total_relation_size is
		 * being executed. For this case we get an empty string as table size.
		 * We can take that as zero to prevent any unnecessary errors.
		 */
		*tableSize = 0;
	}

	PQclear(result);
	ClearResults(connection, failOnError);

	return true;
}


/*
 * GroupShardPlacementsForTableOnGroup accepts a relationId and a group and returns a list
 * of GroupShardPlacement's representing all of the placements for the table which reside
 * on the group.
 */
List *
GroupShardPlacementsForTableOnGroup(Oid relationId, int32 groupId)
{
	CitusTableCacheEntry *distTableCacheEntry = GetCitusTableCacheEntry(relationId);
	List *resultList = NIL;

	int shardIntervalArrayLength = distTableCacheEntry->shardIntervalArrayLength;

	for (int shardIndex = 0; shardIndex < shardIntervalArrayLength; shardIndex++)
	{
		GroupShardPlacement *placementArray =
			distTableCacheEntry->arrayOfPlacementArrays[shardIndex];
		int numberOfPlacements =
			distTableCacheEntry->arrayOfPlacementArrayLengths[shardIndex];

		for (int placementIndex = 0; placementIndex < numberOfPlacements;
			 placementIndex++)
		{
			if (placementArray[placementIndex].groupId == groupId)
			{
				GroupShardPlacement *placement = palloc0(sizeof(GroupShardPlacement));
				*placement = placementArray[placementIndex];
				resultList = lappend(resultList, placement);
			}
		}
	}

	return resultList;
}


/*
 * ShardIntervalsOnWorkerGroup accepts a WorkerNode and returns a list of the shard
 * intervals of the given table which are placed on the group the node is a part of.
 */
static List *
ShardIntervalsOnWorkerGroup(WorkerNode *workerNode, Oid relationId)
{
	CitusTableCacheEntry *distTableCacheEntry = GetCitusTableCacheEntry(relationId);
	List *shardIntervalList = NIL;
	int shardIntervalArrayLength = distTableCacheEntry->shardIntervalArrayLength;

	for (int shardIndex = 0; shardIndex < shardIntervalArrayLength; shardIndex++)
	{
		GroupShardPlacement *placementArray =
			distTableCacheEntry->arrayOfPlacementArrays[shardIndex];
		int numberOfPlacements =
			distTableCacheEntry->arrayOfPlacementArrayLengths[shardIndex];

		for (int placementIndex = 0; placementIndex < numberOfPlacements;
			 placementIndex++)
		{
			GroupShardPlacement *placement = &placementArray[placementIndex];

			if (placement->groupId == workerNode->groupId)
			{
				ShardInterval *cachedShardInterval =
					distTableCacheEntry->sortedShardIntervalArray[shardIndex];
				ShardInterval *shardInterval = CopyShardInterval(cachedShardInterval);
				shardIntervalList = lappend(shardIntervalList, shardInterval);
			}
		}
	}

	return shardIntervalList;
}


/*
 * GenerateSizeQueryOnMultiplePlacements generates a select size query to get
 * size of multiple tables. Note that, different size functions supported by PG
 * are also supported by this function changing the size query type given as the
 * last parameter to function. Depending on the sizeQueryType enum parameter, the
 * generated query will call one of the functions: pg_relation_size,
 * pg_total_relation_size, pg_table_size and cstore_table_size.
 * This function uses UDFs named worker_partitioned_*_size for partitioned tables,
 * if the parameter optimizePartitionCalculations is true. The UDF to be called is
 * determined by the parameter sizeQueryType.
 */
StringInfo
GenerateSizeQueryOnMultiplePlacements(List *shardIntervalList,
									  SizeQueryType sizeQueryType,
									  bool optimizePartitionCalculations)
{
	StringInfo selectQuery = makeStringInfo();

	appendStringInfo(selectQuery, "SELECT ");

	ShardInterval *shardInterval = NULL;
	foreach_ptr(shardInterval, shardIntervalList)
	{
		if (optimizePartitionCalculations && PartitionTable(shardInterval->relationId))
		{
			/*
			 * Skip child tables of a partitioned table as they are already counted in
			 * worker_partitioned_*_size UDFs, if optimizePartitionCalculations is true.
			 * We don't expect this case to happen, since we don't send the child tables
			 * to this function. Because they are all eliminated in
			 * ColocatedNonPartitionShardIntervalList. Therefore we can't cover here with
			 * a test currently. This is added for possible future usages.
			 */
			continue;
		}
		uint64 shardId = shardInterval->shardId;
		Oid schemaId = get_rel_namespace(shardInterval->relationId);
		char *schemaName = get_namespace_name(schemaId);
		char *shardName = get_rel_name(shardInterval->relationId);
		AppendShardIdToName(&shardName, shardId);

		char *shardQualifiedName = quote_qualified_identifier(schemaName, shardName);
		char *quotedShardName = quote_literal_cstr(shardQualifiedName);

		if (optimizePartitionCalculations && PartitionedTable(shardInterval->relationId))
		{
			appendStringInfo(selectQuery, GetWorkerPartitionedSizeUDFNameBySizeQueryType(
								 sizeQueryType), quotedShardName);
		}
		else
		{
			appendStringInfo(selectQuery, GetSizeQueryBySizeQueryType(sizeQueryType),
							 quotedShardName);
		}

		appendStringInfo(selectQuery, " + ");
	}

	/*
	 * Add 0 as a last size, it handles empty list case and makes size control checks
	 * unnecessary which would have implemented without this line.
	 */
	appendStringInfo(selectQuery, "0;");

	return selectQuery;
}


/*
 * GetWorkerPartitionedSizeUDFNameBySizeQueryType returns the corresponding worker
 * partitioned size query for given query type.
 * Errors out for an invalid query type.
 * Currently this function is only called with the type TOTAL_RELATION_SIZE.
 * The others are added for possible future usages. Since they are not used anywhere,
 * currently we can't cover them with tests.
 */
static char *
GetWorkerPartitionedSizeUDFNameBySizeQueryType(SizeQueryType sizeQueryType)
{
	switch (sizeQueryType)
	{
		case RELATION_SIZE:
		{
			return WORKER_PARTITIONED_RELATION_SIZE_FUNCTION;
		}

		case TOTAL_RELATION_SIZE:
		{
			return WORKER_PARTITIONED_RELATION_TOTAL_SIZE_FUNCTION;
		}

		case TABLE_SIZE:
		{
			return WORKER_PARTITIONED_TABLE_SIZE_FUNCTION;
		}

		default:
		{
			elog(ERROR, "Size query type couldn't be found.");
		}
	}
}


/*
 * GetSizeQueryBySizeQueryType returns the corresponding size query for given query type.
 * Errors out for an invalid query type.
 */
static char *
GetSizeQueryBySizeQueryType(SizeQueryType sizeQueryType)
{
	switch (sizeQueryType)
	{
		case RELATION_SIZE:
		{
			return PG_RELATION_SIZE_FUNCTION;
		}

		case TOTAL_RELATION_SIZE:
		{
			return PG_TOTAL_RELATION_SIZE_FUNCTION;
		}

		case TABLE_SIZE:
		{
			return PG_TABLE_SIZE_FUNCTION;
		}

		default:
		{
			elog(ERROR, "Size query type couldn't be found.");
		}
	}
}


/*
 * GenerateAllShardStatisticsQueryForNode generates a query that returns:
 * shard_id, shard_name, shard_size for all shard placements on the node
 */
static char *
GenerateAllShardStatisticsQueryForNode(WorkerNode *workerNode, List *citusTableIds)
{
	StringInfo allShardStatisticsQuery = makeStringInfo();

	Oid relationId = InvalidOid;
	foreach_oid(relationId, citusTableIds)
	{
		/*
		 * Ensure the table still exists by trying to acquire a lock on it
		 * If function returns NULL, it means the table doesn't exist
		 * hence we should skip
		 */
		Relation relation = try_relation_open(relationId, AccessShareLock);
		if (relation != NULL)
		{
			List *shardIntervalsOnNode = ShardIntervalsOnWorkerGroup(workerNode,
																	 relationId);
			char *shardStatisticsQuery =
				GenerateShardStatisticsQueryForShardList(shardIntervalsOnNode);
			appendStringInfoString(allShardStatisticsQuery, shardStatisticsQuery);
			relation_close(relation, AccessShareLock);
		}
	}

	/* Add a dummy entry so that UNION ALL doesn't complain */
	appendStringInfo(allShardStatisticsQuery, "SELECT 0::bigint, NULL::text, 0::bigint;");

	return allShardStatisticsQuery->data;
}


/*
 * GenerateShardStatisticsQueryForShardList generates a query that returns:
 * SELECT shard_id, shard_name, shard_size for all shards in the list
 */
static char *
GenerateShardStatisticsQueryForShardList(List *shardIntervalList)
{
	StringInfo selectQuery = makeStringInfo();

	ShardInterval *shardInterval = NULL;
	foreach_ptr(shardInterval, shardIntervalList)
	{
		AppendShardSizeQuery(selectQuery, shardInterval);
		appendStringInfo(selectQuery, " UNION ALL ");
	}

	return selectQuery->data;
}


/*
 * AppendShardSizeQuery appends a query in the following form to selectQuery
 * SELECT shard_id, shard_name, shard_size
 */
static void
AppendShardSizeQuery(StringInfo selectQuery, ShardInterval *shardInterval)
{
	uint64 shardId = shardInterval->shardId;
	Oid schemaId = get_rel_namespace(shardInterval->relationId);
	char *schemaName = get_namespace_name(schemaId);
	char *shardName = get_rel_name(shardInterval->relationId);

	AppendShardIdToName(&shardName, shardId);

	char *shardQualifiedName = quote_qualified_identifier(schemaName, shardName);
	char *quotedShardName = quote_literal_cstr(shardQualifiedName);

	appendStringInfo(selectQuery, "SELECT " UINT64_FORMAT " AS shard_id, ", shardId);
	appendStringInfo(selectQuery, "%s AS shard_name, ", quotedShardName);
	appendStringInfo(selectQuery, PG_RELATION_SIZE_FUNCTION, quotedShardName);
}


/*
 * ErrorIfNotSuitableToGetSize determines whether the table is suitable to find
 * its' size with internal functions.
 */
static void
ErrorIfNotSuitableToGetSize(Oid relationId)
{
	if (!IsCitusTable(relationId))
	{
		char *relationName = get_rel_name(relationId);
		char *escapedQueryString = quote_literal_cstr(relationName);
		ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						errmsg("cannot calculate the size because relation %s is not "
							   "distributed", escapedQueryString)));
	}
}


/*
 * CompareShardPlacementsByWorker compares two shard placements by their
 * worker node name and port.
 */
int
CompareShardPlacementsByWorker(const void *leftElement, const void *rightElement)
{
	const ShardPlacement *leftPlacement = *((const ShardPlacement **) leftElement);
	const ShardPlacement *rightPlacement = *((const ShardPlacement **) rightElement);

	int nodeNameCmp = strncmp(leftPlacement->nodeName, rightPlacement->nodeName,
							  WORKER_LENGTH);
	if (nodeNameCmp != 0)
	{
		return nodeNameCmp;
	}
	else if (leftPlacement->nodePort > rightPlacement->nodePort)
	{
		return 1;
	}
	else if (leftPlacement->nodePort < rightPlacement->nodePort)
	{
		return -1;
	}

	return 0;
}


/*
 * CompareShardPlacementsByGroupId compares two shard placements by their
 * group id.
 */
int
CompareShardPlacementsByGroupId(const void *leftElement, const void *rightElement)
{
	const ShardPlacement *leftPlacement = *((const ShardPlacement **) leftElement);
	const ShardPlacement *rightPlacement = *((const ShardPlacement **) rightElement);

	if (leftPlacement->groupId > rightPlacement->groupId)
	{
		return 1;
	}
	else if (leftPlacement->groupId < rightPlacement->groupId)
	{
		return -1;
	}
	else
	{
		return 0;
	}
}


/*
 * TableShardReplicationFactor returns the current replication factor of the
 * given relation by looking into shard placements. It errors out if there
 * are different number of shard placements for different shards. It also
 * errors out if the table does not have any shards.
 */
uint32
TableShardReplicationFactor(Oid relationId)
{
	uint32 replicationCount = 0;

	List *shardIntervalList = LoadShardIntervalList(relationId);
	ShardInterval *shardInterval = NULL;
	foreach_ptr(shardInterval, shardIntervalList)
	{
		uint64 shardId = shardInterval->shardId;

		List *shardPlacementList = ShardPlacementListWithoutOrphanedPlacements(shardId);
		uint32 shardPlacementCount = list_length(shardPlacementList);

		/*
		 * Get the replication count of the first shard in the list, and error
		 * out if there is a shard with different replication count.
		 */
		if (replicationCount == 0)
		{
			replicationCount = shardPlacementCount;
		}
		else if (replicationCount != shardPlacementCount)
		{
			char *relationName = get_rel_name(relationId);
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot find the replication factor of the "
								   "table %s", relationName),
							errdetail("The shard " UINT64_FORMAT
									  " has different shards replication counts from "
									  "other shards.", shardId)));
		}
	}

	/* error out if the table does not have any shards */
	if (replicationCount == 0)
	{
		char *relationName = get_rel_name(relationId);
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot find the replication factor of the "
							   "table %s", relationName),
						errdetail("The table %s does not have any shards.",
								  relationName)));
	}

	return replicationCount;
}


/*
 * LoadShardIntervalList returns a list of shard intervals related for a given
 * distributed table. The function returns an empty list if no shards can be
 * found for the given relation.
 * Since LoadShardIntervalList relies on sortedShardIntervalArray, it returns
 * a shard interval list whose elements are sorted on shardminvalue. Shard intervals
 * with uninitialized shard min/max values are placed in the end of the list.
 */
List *
LoadShardIntervalList(Oid relationId)
{
	CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(relationId);
	List *shardList = NIL;

	for (int i = 0; i < cacheEntry->shardIntervalArrayLength; i++)
	{
		ShardInterval *newShardInterval =
			CopyShardInterval(cacheEntry->sortedShardIntervalArray[i]);
		shardList = lappend(shardList, newShardInterval);
	}

	return shardList;
}


/*
 * LoadUnsortedShardIntervalListViaCatalog returns a list of shard intervals related for a
 * given distributed table. The function returns an empty list if no shards can be found
 * for the given relation.
 *
 * This function does not use CitusTableCache and instead reads from catalog tables
 * directly.
 */
List *
LoadUnsortedShardIntervalListViaCatalog(Oid relationId)
{
	List *shardIntervalList = NIL;
	List *distShardTuples = LookupDistShardTuples(relationId);
	Relation distShardRelation = table_open(DistShardRelationId(), AccessShareLock);
	TupleDesc distShardTupleDesc = RelationGetDescr(distShardRelation);
	Oid intervalTypeId = InvalidOid;
	int32 intervalTypeMod = -1;

	char partitionMethod = PartitionMethodViaCatalog(relationId);
	Var *partitionColumn = PartitionColumnViaCatalog(relationId);
	GetIntervalTypeInfo(partitionMethod, partitionColumn, &intervalTypeId,
						&intervalTypeMod);

	HeapTuple distShardTuple = NULL;
	foreach_ptr(distShardTuple, distShardTuples)
	{
		ShardInterval *interval = TupleToShardInterval(distShardTuple,
													   distShardTupleDesc,
													   intervalTypeId,
													   intervalTypeMod);
		shardIntervalList = lappend(shardIntervalList, interval);
	}
	table_close(distShardRelation, AccessShareLock);

	return shardIntervalList;
}


/*
 * LoadShardIntervalWithLongestShardName is a utility function that returns
 * the shard interaval with the largest shardId for the given relationId. Note
 * that largest shardId implies longest shard name.
 */
ShardInterval *
LoadShardIntervalWithLongestShardName(Oid relationId)
{
	CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(relationId);
	int shardIntervalCount = cacheEntry->shardIntervalArrayLength;

	int maxShardIndex = shardIntervalCount - 1;
	uint64 largestShardId = INVALID_SHARD_ID;

	for (int shardIndex = 0; shardIndex <= maxShardIndex; ++shardIndex)
	{
		ShardInterval *currentShardInterval =
			cacheEntry->sortedShardIntervalArray[shardIndex];

		if (largestShardId < currentShardInterval->shardId)
		{
			largestShardId = currentShardInterval->shardId;
		}
	}

	return LoadShardInterval(largestShardId);
}


/*
 * ShardIntervalCount returns number of shard intervals for a given distributed table.
 * The function returns 0 if no shards can be found for the given relation id.
 */
int
ShardIntervalCount(Oid relationId)
{
	CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(relationId);

	return cacheEntry->shardIntervalArrayLength;
}


/*
 * LoadShardList reads list of shards for given relationId from pg_dist_shard,
 * and returns the list of found shardIds.
 * Since LoadShardList relies on sortedShardIntervalArray, it returns a shard
 * list whose elements are sorted on shardminvalue. Shards with uninitialized
 * shard min/max values are placed in the end of the list.
 */
List *
LoadShardList(Oid relationId)
{
	CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(relationId);
	List *shardList = NIL;

	for (int i = 0; i < cacheEntry->shardIntervalArrayLength; i++)
	{
		ShardInterval *currentShardInterval = cacheEntry->sortedShardIntervalArray[i];
		uint64 *shardIdPointer = AllocateUint64(currentShardInterval->shardId);

		shardList = lappend(shardList, shardIdPointer);
	}

	return shardList;
}


/* Allocates eight bytes, and copies given value's contents those bytes. */
static uint64 *
AllocateUint64(uint64 value)
{
	uint64 *allocatedValue = (uint64 *) palloc0(sizeof(uint64));
	Assert(sizeof(uint64) >= 8);

	(*allocatedValue) = value;

	return allocatedValue;
}


/*
 * CopyShardInterval creates a copy of the specified source ShardInterval.
 */
ShardInterval *
CopyShardInterval(ShardInterval *srcInterval)
{
	ShardInterval *destInterval = palloc0(sizeof(ShardInterval));

	destInterval->type = srcInterval->type;
	destInterval->relationId = srcInterval->relationId;
	destInterval->storageType = srcInterval->storageType;
	destInterval->valueTypeId = srcInterval->valueTypeId;
	destInterval->valueTypeLen = srcInterval->valueTypeLen;
	destInterval->valueByVal = srcInterval->valueByVal;
	destInterval->minValueExists = srcInterval->minValueExists;
	destInterval->maxValueExists = srcInterval->maxValueExists;
	destInterval->shardId = srcInterval->shardId;
	destInterval->shardIndex = srcInterval->shardIndex;

	destInterval->minValue = 0;
	if (destInterval->minValueExists)
	{
		destInterval->minValue = datumCopy(srcInterval->minValue,
										   srcInterval->valueByVal,
										   srcInterval->valueTypeLen);
	}

	destInterval->maxValue = 0;
	if (destInterval->maxValueExists)
	{
		destInterval->maxValue = datumCopy(srcInterval->maxValue,
										   srcInterval->valueByVal,
										   srcInterval->valueTypeLen);
	}

	return destInterval;
}


/*
 * ShardLength finds shard placements for the given shardId, extracts the length
 * of an active shard, and returns the shard's length. This function errors
 * out if we cannot find any active shard placements for the given shardId.
 */
uint64
ShardLength(uint64 shardId)
{
	uint64 shardLength = 0;

	List *shardPlacementList = ActiveShardPlacementList(shardId);
	if (shardPlacementList == NIL)
	{
		ereport(ERROR, (errmsg("could not find length of shard " UINT64_FORMAT, shardId),
						errdetail("Could not find any shard placements for the shard.")));
	}
	else
	{
		ShardPlacement *shardPlacement = (ShardPlacement *) linitial(shardPlacementList);
		shardLength = shardPlacement->shardLength;
	}

	return shardLength;
}


/*
 * NodeGroupHasShardPlacements returns whether any active shards are placed on the group
 */
bool
NodeGroupHasShardPlacements(int32 groupId, bool onlyConsiderActivePlacements)
{
	const int scanKeyCount = (onlyConsiderActivePlacements ? 2 : 1);
	const bool indexOK = false;


	ScanKeyData scanKey[2];

	Relation pgPlacement = table_open(DistPlacementRelationId(),
									  AccessShareLock);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_placement_groupid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(groupId));
	if (onlyConsiderActivePlacements)
	{
		ScanKeyInit(&scanKey[1], Anum_pg_dist_placement_shardstate,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(SHARD_STATE_ACTIVE));
	}

	SysScanDesc scanDescriptor = systable_beginscan(pgPlacement,
													DistPlacementGroupidIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	bool hasActivePlacements = HeapTupleIsValid(heapTuple);

	systable_endscan(scanDescriptor);
	table_close(pgPlacement, NoLock);

	return hasActivePlacements;
}


/*
 * ActiveShardPlacementListOnGroup returns a list of active shard placements
 * that are sitting on group with groupId for given shardId.
 */
List *
ActiveShardPlacementListOnGroup(uint64 shardId, int32 groupId)
{
	List *activeShardPlacementListOnGroup = NIL;

	List *activePlacementList = ActiveShardPlacementList(shardId);
	ShardPlacement *shardPlacement = NULL;
	foreach_ptr(shardPlacement, activePlacementList)
	{
		if (shardPlacement->groupId == groupId)
		{
			activeShardPlacementListOnGroup = lappend(activeShardPlacementListOnGroup,
													  shardPlacement);
		}
	}

	return activeShardPlacementListOnGroup;
}


/*
 * ActiveShardPlacementList finds shard placements for the given shardId from
 * system catalogs, chooses placements that are in active state, and returns
 * these shard placements in a new list.
 */
List *
ActiveShardPlacementList(uint64 shardId)
{
	List *activePlacementList = NIL;
	List *shardPlacementList =
		ShardPlacementListIncludingOrphanedPlacements(shardId);

	ShardPlacement *shardPlacement = NULL;
	foreach_ptr(shardPlacement, shardPlacementList)
	{
		WorkerNode *workerNode =
			FindWorkerNode(shardPlacement->nodeName, shardPlacement->nodePort);

		/*
		 * We have already resolved the placement to node, so would have
		 * errored out earlier.
		 */
		Assert(workerNode != NULL);

		if (shardPlacement->shardState == SHARD_STATE_ACTIVE &&
			workerNode->isActive)
		{
			activePlacementList = lappend(activePlacementList, shardPlacement);
		}
	}

	return SortList(activePlacementList, CompareShardPlacementsByWorker);
}


/*
 * ShardPlacementListWithoutOrphanedPlacements returns shard placements exluding
 * the ones that are orphaned, because they are marked to be deleted at a later
 * point (shardstate = 4).
 */
List *
ShardPlacementListWithoutOrphanedPlacements(uint64 shardId)
{
	List *activePlacementList = NIL;
	List *shardPlacementList =
		ShardPlacementListIncludingOrphanedPlacements(shardId);

	ShardPlacement *shardPlacement = NULL;
	foreach_ptr(shardPlacement, shardPlacementList)
	{
		if (shardPlacement->shardState != SHARD_STATE_TO_DELETE)
		{
			activePlacementList = lappend(activePlacementList, shardPlacement);
		}
	}

	return SortList(activePlacementList, CompareShardPlacementsByWorker);
}


/*
 * ActiveShardPlacement finds a shard placement for the given shardId from
 * system catalog, chooses a placement that is in active state and returns
 * that shard placement. If this function cannot find a healthy shard placement
 * and missingOk is set to false it errors out.
 */
ShardPlacement *
ActiveShardPlacement(uint64 shardId, bool missingOk)
{
	List *activePlacementList = ActiveShardPlacementList(shardId);
	ShardPlacement *shardPlacement = NULL;

	if (list_length(activePlacementList) == 0)
	{
		if (!missingOk)
		{
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("could not find any healthy placement for shard "
								   UINT64_FORMAT, shardId)));
		}

		return shardPlacement;
	}

	shardPlacement = (ShardPlacement *) linitial(activePlacementList);

	return shardPlacement;
}


/*
 * BuildShardPlacementList finds shard placements for the given shardId from
 * system catalogs, converts these placements to their in-memory
 * representation, and returns the converted shard placements in a new list.
 *
 * This probably only should be called from metadata_cache.c.  Resides here
 * because it shares code with other routines in this file.
 */
List *
BuildShardPlacementList(int64 shardId)
{
	List *shardPlacementList = NIL;
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	bool indexOK = true;

	Relation pgPlacement = table_open(DistPlacementRelationId(), AccessShareLock);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_placement_shardid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(shardId));

	SysScanDesc scanDescriptor = systable_beginscan(pgPlacement,
													DistPlacementShardidIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	while (HeapTupleIsValid(heapTuple))
	{
		TupleDesc tupleDescriptor = RelationGetDescr(pgPlacement);

		GroupShardPlacement *placement =
			TupleToGroupShardPlacement(tupleDescriptor, heapTuple);

		shardPlacementList = lappend(shardPlacementList, placement);

		heapTuple = systable_getnext(scanDescriptor);
	}

	systable_endscan(scanDescriptor);
	table_close(pgPlacement, NoLock);

	return shardPlacementList;
}


/*
 * BuildShardPlacementListForGroup finds shard placements for the given groupId
 * from system catalogs, converts these placements to their in-memory
 * representation, and returns the converted shard placements in a new list.
 */
List *
AllShardPlacementsOnNodeGroup(int32 groupId)
{
	List *shardPlacementList = NIL;
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	bool indexOK = true;

	Relation pgPlacement = table_open(DistPlacementRelationId(), AccessShareLock);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_placement_groupid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(groupId));

	SysScanDesc scanDescriptor = systable_beginscan(pgPlacement,
													DistPlacementGroupidIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	while (HeapTupleIsValid(heapTuple))
	{
		TupleDesc tupleDescriptor = RelationGetDescr(pgPlacement);

		GroupShardPlacement *placement =
			TupleToGroupShardPlacement(tupleDescriptor, heapTuple);

		shardPlacementList = lappend(shardPlacementList, placement);

		heapTuple = systable_getnext(scanDescriptor);
	}

	systable_endscan(scanDescriptor);
	table_close(pgPlacement, NoLock);

	return shardPlacementList;
}


/*
 * AllShardPlacementsWithShardPlacementState finds shard placements with the given
 * shardState from system catalogs, converts these placements to their in-memory
 * representation, and returns the converted shard placements in a new list.
 */
List *
AllShardPlacementsWithShardPlacementState(ShardState shardState)
{
	List *shardPlacementList = NIL;
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;

	Relation pgPlacement = table_open(DistPlacementRelationId(), AccessShareLock);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_placement_shardstate,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(shardState));

	SysScanDesc scanDescriptor = systable_beginscan(pgPlacement, InvalidOid, false,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	while (HeapTupleIsValid(heapTuple))
	{
		TupleDesc tupleDescriptor = RelationGetDescr(pgPlacement);

		GroupShardPlacement *placement =
			TupleToGroupShardPlacement(tupleDescriptor, heapTuple);

		shardPlacementList = lappend(shardPlacementList, placement);

		heapTuple = systable_getnext(scanDescriptor);
	}

	systable_endscan(scanDescriptor);
	table_close(pgPlacement, NoLock);

	return shardPlacementList;
}


/*
 * TupleToGroupShardPlacement takes in a heap tuple from pg_dist_placement,
 * and converts this tuple to in-memory struct. The function assumes the
 * caller already has locks on the tuple, and doesn't perform any locking.
 */
static GroupShardPlacement *
TupleToGroupShardPlacement(TupleDesc tupleDescriptor, HeapTuple heapTuple)
{
	bool isNullArray[Natts_pg_dist_placement];
	Datum datumArray[Natts_pg_dist_placement];

	if (HeapTupleHeaderGetNatts(heapTuple->t_data) != Natts_pg_dist_placement ||
		HeapTupleHasNulls(heapTuple))
	{
		ereport(ERROR, (errmsg("unexpected null in pg_dist_placement tuple")));
	}

	/*
	 * We use heap_deform_tuple() instead of heap_getattr() to expand tuple
	 * to contain missing values when ALTER TABLE ADD COLUMN happens.
	 */
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);

	GroupShardPlacement *shardPlacement = CitusMakeNode(GroupShardPlacement);
	shardPlacement->placementId = DatumGetInt64(
		datumArray[Anum_pg_dist_placement_placementid - 1]);
	shardPlacement->shardId = DatumGetInt64(
		datumArray[Anum_pg_dist_placement_shardid - 1]);
	shardPlacement->shardLength = DatumGetInt64(
		datumArray[Anum_pg_dist_placement_shardlength - 1]);
	shardPlacement->shardState = DatumGetUInt32(
		datumArray[Anum_pg_dist_placement_shardstate - 1]);
	shardPlacement->groupId = DatumGetInt32(
		datumArray[Anum_pg_dist_placement_groupid - 1]);

	return shardPlacement;
}


/*
 * InsertShardRow opens the shard system catalog, and inserts a new row with the
 * given values into that system catalog. Note that we allow the user to pass in
 * null min/max values in case they are creating an empty shard.
 */
void
InsertShardRow(Oid relationId, uint64 shardId, char storageType,
			   text *shardMinValue, text *shardMaxValue)
{
	Datum values[Natts_pg_dist_shard];
	bool isNulls[Natts_pg_dist_shard];

	/* form new shard tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[Anum_pg_dist_shard_logicalrelid - 1] = ObjectIdGetDatum(relationId);
	values[Anum_pg_dist_shard_shardid - 1] = Int64GetDatum(shardId);
	values[Anum_pg_dist_shard_shardstorage - 1] = CharGetDatum(storageType);

	/* dropped shardalias column must also be set; it is still part of the tuple */
	isNulls[Anum_pg_dist_shard_shardalias_DROPPED - 1] = true;

	/* check if shard min/max values are null */
	if (shardMinValue != NULL && shardMaxValue != NULL)
	{
		values[Anum_pg_dist_shard_shardminvalue - 1] = PointerGetDatum(shardMinValue);
		values[Anum_pg_dist_shard_shardmaxvalue - 1] = PointerGetDatum(shardMaxValue);
	}
	else
	{
		isNulls[Anum_pg_dist_shard_shardminvalue - 1] = true;
		isNulls[Anum_pg_dist_shard_shardmaxvalue - 1] = true;
	}

	/* open shard relation and insert new tuple */
	Relation pgDistShard = table_open(DistShardRelationId(), RowExclusiveLock);

	TupleDesc tupleDescriptor = RelationGetDescr(pgDistShard);
	HeapTuple heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	CatalogTupleInsert(pgDistShard, heapTuple);

	/* invalidate previous cache entry and close relation */
	CitusInvalidateRelcacheByRelid(relationId);

	CommandCounterIncrement();
	table_close(pgDistShard, NoLock);
}


/*
 * InsertShardPlacementRow opens the shard placement system catalog, and inserts
 * a new row with the given values into that system catalog. If placementId is
 * INVALID_PLACEMENT_ID, a new placement id will be assigned.Then, returns the
 * placement id of the added shard placement.
 */
uint64
InsertShardPlacementRow(uint64 shardId, uint64 placementId,
						char shardState, uint64 shardLength,
						int32 groupId)
{
	Datum values[Natts_pg_dist_placement];
	bool isNulls[Natts_pg_dist_placement];

	/* form new shard placement tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	if (placementId == INVALID_PLACEMENT_ID)
	{
		placementId = master_get_new_placementid(NULL);
	}
	values[Anum_pg_dist_placement_placementid - 1] = Int64GetDatum(placementId);
	values[Anum_pg_dist_placement_shardid - 1] = Int64GetDatum(shardId);
	values[Anum_pg_dist_placement_shardstate - 1] = CharGetDatum(shardState);
	values[Anum_pg_dist_placement_shardlength - 1] = Int64GetDatum(shardLength);
	values[Anum_pg_dist_placement_groupid - 1] = Int32GetDatum(groupId);

	/* open shard placement relation and insert new tuple */
	Relation pgDistPlacement = table_open(DistPlacementRelationId(), RowExclusiveLock);

	TupleDesc tupleDescriptor = RelationGetDescr(pgDistPlacement);
	HeapTuple heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	CatalogTupleInsert(pgDistPlacement, heapTuple);

	CitusInvalidateRelcacheByShardId(shardId);

	CommandCounterIncrement();
	table_close(pgDistPlacement, NoLock);

	return placementId;
}


/*
 * InsertIntoPgDistPartition inserts a new tuple into pg_dist_partition.
 */
void
InsertIntoPgDistPartition(Oid relationId, char distributionMethod,
						  Var *distributionColumn, uint32 colocationId,
						  char replicationModel, bool autoConverted)
{
	char *distributionColumnString = NULL;

	Datum newValues[Natts_pg_dist_partition];
	bool newNulls[Natts_pg_dist_partition];

	/* open system catalog and insert new tuple */
	Relation pgDistPartition = table_open(DistPartitionRelationId(), RowExclusiveLock);

	/* form new tuple for pg_dist_partition */
	memset(newValues, 0, sizeof(newValues));
	memset(newNulls, false, sizeof(newNulls));

	newValues[Anum_pg_dist_partition_logicalrelid - 1] =
		ObjectIdGetDatum(relationId);
	newValues[Anum_pg_dist_partition_partmethod - 1] =
		CharGetDatum(distributionMethod);
	newValues[Anum_pg_dist_partition_colocationid - 1] = UInt32GetDatum(colocationId);
	newValues[Anum_pg_dist_partition_repmodel - 1] = CharGetDatum(replicationModel);
	newValues[Anum_pg_dist_partition_autoconverted - 1] = BoolGetDatum(autoConverted);

	/* set partkey column to NULL for reference tables */
	if (distributionMethod != DISTRIBUTE_BY_NONE)
	{
		distributionColumnString = nodeToString((Node *) distributionColumn);

		newValues[Anum_pg_dist_partition_partkey - 1] =
			CStringGetTextDatum(distributionColumnString);
	}
	else
	{
		newValues[Anum_pg_dist_partition_partkey - 1] = PointerGetDatum(NULL);
		newNulls[Anum_pg_dist_partition_partkey - 1] = true;
	}

	HeapTuple newTuple = heap_form_tuple(RelationGetDescr(pgDistPartition), newValues,
										 newNulls);

	/* finally insert tuple, build index entries & register cache invalidation */
	CatalogTupleInsert(pgDistPartition, newTuple);

	CitusInvalidateRelcacheByRelid(relationId);

	RecordDistributedRelationDependencies(relationId);

	CommandCounterIncrement();
	table_close(pgDistPartition, NoLock);
}


/*
 * RecordDistributedRelationDependencies creates the dependency entries
 * necessary for a distributed relation in addition to the preexisting ones
 * for a normal relation.
 *
 * We create one dependency from the (now distributed) relation to the citus
 * extension to prevent the extension from being dropped while distributed
 * tables exist. Furthermore a dependency from pg_dist_partition's
 * distribution clause to the underlying columns is created, but it's marked
 * as being owned by the relation itself. That means the entire table can be
 * dropped, but the column itself can't. Neither can the type of the
 * distribution column be changed (c.f. ATExecAlterColumnType).
 */
static void
RecordDistributedRelationDependencies(Oid distributedRelationId)
{
	ObjectAddress relationAddr = { 0, 0, 0 };
	ObjectAddress citusExtensionAddr = { 0, 0, 0 };

	relationAddr.classId = RelationRelationId;
	relationAddr.objectId = distributedRelationId;
	relationAddr.objectSubId = 0;

	citusExtensionAddr.classId = ExtensionRelationId;
	citusExtensionAddr.objectId = get_extension_oid("citus", false);
	citusExtensionAddr.objectSubId = 0;

	/* dependency from table entry to extension */
	recordDependencyOn(&relationAddr, &citusExtensionAddr, DEPENDENCY_NORMAL);
}


/*
 * DeletePartitionRow removes the row from pg_dist_partition where the logicalrelid
 * field equals to distributedRelationId. Then, the function invalidates the
 * metadata cache.
 */
void
DeletePartitionRow(Oid distributedRelationId)
{
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;

	Relation pgDistPartition = table_open(DistPartitionRelationId(), RowExclusiveLock);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_partition_logicalrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(distributedRelationId));

	SysScanDesc scanDescriptor = systable_beginscan(pgDistPartition, InvalidOid, false,
													NULL,
													scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for partition %d",
							   distributedRelationId)));
	}

	simple_heap_delete(pgDistPartition, &heapTuple->t_self);

	systable_endscan(scanDescriptor);

	/* invalidate the cache */
	CitusInvalidateRelcacheByRelid(distributedRelationId);

	/* increment the counter so that next command can see the row */
	CommandCounterIncrement();

	table_close(pgDistPartition, NoLock);
}


/*
 * DeleteShardRow opens the shard system catalog, finds the unique row that has
 * the given shardId, and deletes this row.
 */
void
DeleteShardRow(uint64 shardId)
{
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	bool indexOK = true;

	Relation pgDistShard = table_open(DistShardRelationId(), RowExclusiveLock);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_shard_shardid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(shardId));

	SysScanDesc scanDescriptor = systable_beginscan(pgDistShard,
													DistShardShardidIndexId(), indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for shard "
							   UINT64_FORMAT, shardId)));
	}

	Form_pg_dist_shard pgDistShardForm = (Form_pg_dist_shard) GETSTRUCT(heapTuple);
	Oid distributedRelationId = pgDistShardForm->logicalrelid;

	simple_heap_delete(pgDistShard, &heapTuple->t_self);

	systable_endscan(scanDescriptor);

	/* invalidate previous cache entry */
	CitusInvalidateRelcacheByRelid(distributedRelationId);

	CommandCounterIncrement();
	table_close(pgDistShard, NoLock);
}


/*
 * DeleteShardPlacementRow opens the shard placement system catalog, finds the placement
 * with the given placementId, and deletes it.
 */
void
DeleteShardPlacementRow(uint64 placementId)
{
	const int scanKeyCount = 1;
	ScanKeyData scanKey[1];
	bool indexOK = true;
	bool isNull = false;

	Relation pgDistPlacement = table_open(DistPlacementRelationId(), RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistPlacement);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_placement_placementid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(placementId));

	SysScanDesc scanDescriptor = systable_beginscan(pgDistPlacement,
													DistPlacementPlacementidIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (heapTuple == NULL)
	{
		ereport(ERROR, (errmsg("could not find valid entry for shard placement "
							   INT64_FORMAT, placementId)));
	}

	uint64 shardId = heap_getattr(heapTuple, Anum_pg_dist_placement_shardid,
								  tupleDescriptor, &isNull);
	if (HeapTupleHeaderGetNatts(heapTuple->t_data) != Natts_pg_dist_placement ||
		HeapTupleHasNulls(heapTuple))
	{
		ereport(ERROR, (errmsg("unexpected null in pg_dist_placement tuple")));
	}

	simple_heap_delete(pgDistPlacement, &heapTuple->t_self);
	systable_endscan(scanDescriptor);

	CitusInvalidateRelcacheByShardId(shardId);

	CommandCounterIncrement();
	table_close(pgDistPlacement, NoLock);
}


/*
 * UpdateShardPlacementState sets the shardState for the placement identified
 * by placementId.
 */
void
UpdateShardPlacementState(uint64 placementId, char shardState)
{
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	bool indexOK = true;
	Datum values[Natts_pg_dist_placement];
	bool isnull[Natts_pg_dist_placement];
	bool replace[Natts_pg_dist_placement];
	bool colIsNull = false;

	Relation pgDistPlacement = table_open(DistPlacementRelationId(), RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistPlacement);
	ScanKeyInit(&scanKey[0], Anum_pg_dist_placement_placementid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(placementId));

	SysScanDesc scanDescriptor = systable_beginscan(pgDistPlacement,
													DistPlacementPlacementidIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for shard placement "
							   UINT64_FORMAT,
							   placementId)));
	}

	memset(replace, 0, sizeof(replace));

	values[Anum_pg_dist_placement_shardstate - 1] = CharGetDatum(shardState);
	isnull[Anum_pg_dist_placement_shardstate - 1] = false;
	replace[Anum_pg_dist_placement_shardstate - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(pgDistPlacement, &heapTuple->t_self, heapTuple);

	uint64 shardId = DatumGetInt64(heap_getattr(heapTuple,
												Anum_pg_dist_placement_shardid,
												tupleDescriptor, &colIsNull));
	Assert(!colIsNull);
	CitusInvalidateRelcacheByShardId(shardId);

	CommandCounterIncrement();

	systable_endscan(scanDescriptor);
	table_close(pgDistPlacement, NoLock);
}


/*
 * UpdatePlacementGroupId sets the groupId for the placement identified
 * by placementId.
 */
void
UpdatePlacementGroupId(uint64 placementId, int groupId)
{
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	bool indexOK = true;
	Datum values[Natts_pg_dist_placement];
	bool isnull[Natts_pg_dist_placement];
	bool replace[Natts_pg_dist_placement];
	bool colIsNull = false;

	Relation pgDistPlacement = table_open(DistPlacementRelationId(), RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistPlacement);
	ScanKeyInit(&scanKey[0], Anum_pg_dist_placement_placementid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(placementId));

	SysScanDesc scanDescriptor = systable_beginscan(pgDistPlacement,
													DistPlacementPlacementidIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for shard placement "
							   UINT64_FORMAT,
							   placementId)));
	}

	memset(replace, 0, sizeof(replace));

	values[Anum_pg_dist_placement_groupid - 1] = Int32GetDatum(groupId);
	isnull[Anum_pg_dist_placement_groupid - 1] = false;
	replace[Anum_pg_dist_placement_groupid - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(pgDistPlacement, &heapTuple->t_self, heapTuple);

	uint64 shardId = DatumGetInt64(heap_getattr(heapTuple,
												Anum_pg_dist_placement_shardid,
												tupleDescriptor, &colIsNull));
	Assert(!colIsNull);
	CitusInvalidateRelcacheByShardId(shardId);

	CommandCounterIncrement();

	systable_endscan(scanDescriptor);
	table_close(pgDistPlacement, NoLock);
}


/*
 * UpdatePgDistPartitionAutoConverted sets the autoConverted for the partition identified
 * by citusTableId.
 */
void
UpdatePgDistPartitionAutoConverted(Oid citusTableId, bool autoConverted)
{
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	bool indexOK = true;
	Datum values[Natts_pg_dist_partition];
	bool isnull[Natts_pg_dist_partition];
	bool replace[Natts_pg_dist_partition];

	Relation pgDistPartition = table_open(DistPartitionRelationId(), RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistPartition);
	ScanKeyInit(&scanKey[0], Anum_pg_dist_partition_logicalrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(citusTableId));

	SysScanDesc scanDescriptor = systable_beginscan(pgDistPartition,
													DistPartitionLogicalRelidIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for citus table with oid: %u",
							   citusTableId)));
	}

	memset(replace, 0, sizeof(replace));

	values[Anum_pg_dist_partition_autoconverted - 1] = BoolGetDatum(autoConverted);
	isnull[Anum_pg_dist_partition_autoconverted - 1] = false;
	replace[Anum_pg_dist_partition_autoconverted - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(pgDistPartition, &heapTuple->t_self, heapTuple);

	CitusInvalidateRelcacheByRelid(citusTableId);

	CommandCounterIncrement();

	systable_endscan(scanDescriptor);
	table_close(pgDistPartition, NoLock);
}


/*
 * Check that the current user has `mode` permissions on relationId, error out
 * if not. Superusers always have such permissions.
 */
void
EnsureTablePermissions(Oid relationId, AclMode mode)
{
	AclResult aclresult = pg_class_aclcheck(relationId, GetUserId(), mode);

	if (aclresult != ACLCHECK_OK)
	{
		aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(relationId));
	}
}


/*
 * Check that the current user has owner rights to relationId, error out if
 * not. Superusers are regarded as owners.
 */
void
EnsureTableOwner(Oid relationId)
{
	if (!pg_class_ownercheck(relationId, GetUserId()))
	{
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE,
					   get_rel_name(relationId));
	}
}


/*
 * Check that the current user has owner rights to the schema, error out if
 * not. Superusers are regarded as owners.
 */
void
EnsureSchemaOwner(Oid schemaId)
{
	if (!pg_namespace_ownercheck(schemaId, GetUserId()))
	{
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA,
					   get_namespace_name(schemaId));
	}
}


/*
 * Check that the current user has owner rights to functionId, error out if
 * not. Superusers are regarded as owners. Functions and procedures are
 * treated equally.
 */
void
EnsureFunctionOwner(Oid functionId)
{
	if (!pg_proc_ownercheck(functionId, GetUserId()))
	{
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   get_func_name(functionId));
	}
}


/*
 * EnsureHashDistributedTable error out if the given relation is not a hash distributed table
 * with the given message.
 */
void
EnsureHashDistributedTable(Oid relationId)
{
	if (!IsCitusTableType(relationId, HASH_DISTRIBUTED))
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("relation %s should be a "
							   "hash distributed table", get_rel_name(relationId))));
	}
}


/*
 * EnsureSuperUser check that the current user is a superuser and errors out if not.
 */
void
EnsureSuperUser(void)
{
	if (!superuser())
	{
		ereport(ERROR, (errmsg("operation is not allowed"),
						errhint("Run the command with a superuser.")));
	}
}


/*
 * Return a table's owner as a string.
 */
char *
TableOwner(Oid relationId)
{
	HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
						errmsg("relation with OID %u does not exist", relationId)));
	}

	Oid userId = ((Form_pg_class) GETSTRUCT(tuple))->relowner;

	ReleaseSysCache(tuple);

	return GetUserNameFromId(userId, false);
}
