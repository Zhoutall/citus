/*-------------------------------------------------------------------------
 *
 * foreign_server.c
 *    Commands for FOREIGN SERVER statements.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_foreign_server.h"
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
PreprocessCreateForeignServerStmt(Node *node, const char *queryString,
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
PreprocessDropForeignServerStmt(Node *node, const char *queryString,
								ProcessUtilityContext processUtilityContext)
{
	DropStmt *stmt = castNode(DropStmt, node);
	Assert(stmt->removeType == OBJECT_FOREIGN_SERVER);

	List *allServerNamesToDrop = stmt->objects;
	List *distributedServerAddresses = NIL;
	List *distributedServerNames = NIL;
	Value *serverValue = NULL;
	foreach_ptr(serverValue, stmt->objects)
	{
		char *serverString = strVal(serverValue);
		ForeignServer *server = GetForeignServerByName(serverString, false);

		ObjectAddress address = { 0 };
		ObjectAddressSet(address, ForeignServerRelationId, server->serverid);

		/* filter distributed servers */
		if (IsObjectDistributed(&address))
		{
			distributedServerAddresses = lappend(distributedServerAddresses, &address);
			distributedServerNames = lappend(distributedServerNames, serverValue);
		}
	}

	if (list_length(distributedServerNames) <= 0)
	{
		return NIL;
	}

	if (!ShouldPropagate())
	{
		return NIL;
	}

	EnsureCoordinator();

	/* unmark each distributed server */
	ObjectAddress *address = NULL;
	foreach_ptr(address, distributedServerAddresses)
	{
		UnmarkObjectDistributed(address);
	}

	/*
	 * Temporary swap the lists of objects to delete with the distributed
	 * objects and deparse to an sql statement for the workers.
	 * Then switch back to allServerNamesToDrop to drop all specified
	 * servers in coordinator after PreprocessDropForeignServerStmt completes
	 * its execution.
	 */
	stmt->objects = distributedServerNames;
	const char *deparsedStmt = DeparseTreeNode((Node *) stmt);
	stmt->objects = allServerNamesToDrop;

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
PostprocessCreateForeignServerStmt(Node *node, const char *queryString)
{
	ObjectAddress typeAddress = GetObjectAddressFromParseTree(node, false);
	EnsureDependenciesExistOnAllNodes(&typeAddress);

	MarkObjectDistributed(&typeAddress);

	return NIL;
}


ObjectAddress
CreateForeignServerStmtObjectAddress(Node *node, bool missing_ok)
{
	CreateForeignServerStmt *stmt = castNode(CreateForeignServerStmt, node);
	ForeignServer *server = GetForeignServerByName(stmt->servername, false);
	Oid serverOid = server->serverid;
	ObjectAddress address = { 0 };
	ObjectAddressSet(address, ForeignServerRelationId, serverOid);

	return address;
}
