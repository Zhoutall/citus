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
#include "distributed/metadata/distobject.h"
#include "distributed/metadata_sync.h"
#include "distributed/worker_transaction.h"
#include "distributed/worker_create_or_replace.h"
#include "foreign/foreign.h"


List *
PreprocessCreateForeignServerStmt(Node *node, const char *queryString,
								  ProcessUtilityContext processUtilityContext)
{
	EnsureCoordinator();

	/* to prevent recursion with mx we disable ddl propagation */
	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) queryString,
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
