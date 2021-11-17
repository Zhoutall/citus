/*-------------------------------------------------------------------------
 *
 * deparse_fdw_stmts.c
 *	  All routines to deparse foreign data wrapper statements.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "distributed/citus_ruleutils.h"
#include "distributed/deparser.h"
#include "distributed/listutils.h"
#include "distributed/relay_utility.h"
#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "utils/builtins.h"

static void AppendDropFdwStmt(StringInfo buf, DropStmt *stmt);
static void AppendFdwNames(StringInfo buf, DropStmt *stmt);
static void AppendBehavior(StringInfo buf, DropStmt *stmt);

char *
DeparseDropFdwStmt(Node *node)
{
	DropStmt *stmt = castNode(DropStmt, node);

	Assert(stmt->removeType == OBJECT_FDW);

	StringInfoData str;
	initStringInfo(&str);

	AppendDropFdwStmt(&str, stmt);

	return str.data;
}


static void
AppendDropFdwStmt(StringInfo buf, DropStmt *stmt)
{
	appendStringInfoString(buf, "DROP FOREIGN DATA WRAPPER ");

	if (stmt->missing_ok)
	{
		appendStringInfoString(buf, "IF EXISTS ");
	}

	AppendFdwNames(buf, stmt);

	AppendBehavior(buf, stmt);
}


static void
AppendFdwNames(StringInfo buf, DropStmt *stmt)
{
	Value *fdwValue = NULL;
	foreach_ptr(fdwValue, stmt->objects)
	{
		char *fdwString = strVal(fdwValue);
		appendStringInfo(buf, "%s", fdwString);

		if (fdwValue != llast(stmt->objects))
		{
			appendStringInfoString(buf, ", ");
		}
	}
}


static void
AppendBehavior(StringInfo buf, DropStmt *stmt)
{
	if (stmt->behavior == DROP_CASCADE)
	{
		appendStringInfoString(buf, " CASCADE");
	}
	else if (stmt->behavior == DROP_RESTRICT)
	{
		appendStringInfoString(buf, " RESTRICT");
	}
}
