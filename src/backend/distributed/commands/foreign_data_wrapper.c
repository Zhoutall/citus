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
	EnsureCoordinator();

	/* to prevent recursion with mx we disable ddl propagation */
	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) queryString,
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
