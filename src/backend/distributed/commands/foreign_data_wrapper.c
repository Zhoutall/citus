/*-------------------------------------------------------------------------
 *
 * foreign_data_wrapper.c
 *    Commands for FOREIGN DATA WRAPPER statements.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_foreign_data_wrapper.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/commands.h"
#include "distributed/deparser.h"
#include "distributed/listutils.h"
#include "distributed/metadata/distobject.h"
#include "distributed/metadata_sync.h"
#include "distributed/worker_transaction.h"
#include "foreign/foreign.h"
#include "nodes/primnodes.h"

List *
PreprocessCreateFdwStmt(Node *node, const char *queryString,
						ProcessUtilityContext processUtilityContext)
{
	if (!ShouldPropagate())
	{
		return NIL;
	}

	EnsureCoordinator();

	/* to prevent recursion with mx we disable ddl propagation */
	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) queryString,
								ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(NON_COORDINATOR_NODES, commands);
}


List *
PreprocessDropFdwStmt(Node *node, const char *queryString,
					  ProcessUtilityContext processUtilityContext)
{
	DropStmt *stmt = castNode(DropStmt, node);
	Assert(stmt->removeType == OBJECT_FDW);

	List *allFdwNamesToDrop = stmt->objects;
	List *distributedFdwAddresses = NIL;
	List *distributedFdwNames = NIL;
	Value *fdwValue = NULL;
	foreach_ptr(fdwValue, stmt->objects)
	{
		char *fdwString = strVal(fdwValue);
		ForeignDataWrapper *fdw = GetForeignDataWrapperByName(fdwString, false);

		ObjectAddress address = { 0 };
		ObjectAddressSet(address, ForeignDataWrapperRelationId, fdw->fdwid);

		/* filter distributed fdws */
		if (IsObjectDistributed(&address))
		{
			distributedFdwAddresses = lappend(distributedFdwAddresses, &address);
			distributedFdwNames = lappend(distributedFdwNames, fdwValue);
		}
	}

	if (list_length(distributedFdwNames) <= 0)
	{
		return NIL;
	}

	if (!ShouldPropagate())
	{
		return NIL;
	}

	EnsureCoordinator();

	/* unmark each distributed fdw */
	ObjectAddress *address = NULL;
	foreach_ptr(address, distributedFdwAddresses)
	{
		UnmarkObjectDistributed(address);
	}

	/*
	 * Temporary swap the lists of objects to delete with the distributed
	 * objects and deparse to an sql statement for the workers.
	 * Then switch back to allFdwNamesToDrop to drop all specified
	 * servers in coordinator after PreprocessDropForeignServerStmt completes
	 * its execution.
	 */
	stmt->objects = distributedFdwNames;
	const char *deparsedStmt = DeparseTreeNode((Node *) stmt);
	stmt->objects = allFdwNamesToDrop;

	/*
	 * To prevent recursive propagation in mx architecture, we disable ddl
	 * propagation before sending the command to workers.
	 */
	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) deparsedStmt,
								ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(NON_COORDINATOR_NODES, commands);
}


List *
PostprocessCreateFdwStmt(Node *node, const char *queryString)
{
	ObjectAddress typeAddress = GetObjectAddressFromParseTree(node, false);
	EnsureDependenciesExistOnAllNodes(&typeAddress);

	MarkObjectDistributed(&typeAddress);

	return NIL;
}


ObjectAddress
CreateFdwStmtObjectAddress(Node *node, bool missing_ok)
{
	CreateFdwStmt *stmt = castNode(CreateFdwStmt, node);
	ForeignDataWrapper *fdw = GetForeignDataWrapperByName(stmt->fdwname, false);
	Oid fdwOid = fdw->fdwid;
	ObjectAddress address = { 0 };
	ObjectAddressSet(address, ForeignDataWrapperRelationId, fdwOid);

	return address;
}
