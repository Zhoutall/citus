/*-------------------------------------------------------------------------
 *
 * deparse_foreign_server_stmts.c
 *	  All routines to deparse foreign server statements.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "distributed/citus_ruleutils.h"
#include "distributed/deparser.h"
#include "distributed/listutils.h"
#include "distributed/relay_utility.h"
#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "utils/builtins.h"

static void AppendCreateForeignServerStmt(StringInfo buf, CreateForeignServerStmt *stmt);
static void AppendCreateForeignServerOptions(StringInfo buf,
											 CreateForeignServerStmt *stmt);
static void AppendDropForeignServerStmt(StringInfo buf, DropStmt *stmt);
static void AppendServerNames(StringInfo buf, DropStmt *stmt);
static void AppendBehavior(StringInfo buf, DropStmt *stmt);

char *
DeparseCreateForeignServerStmt(Node *node)
{
	CreateForeignServerStmt *stmt = castNode(CreateForeignServerStmt, node);

	StringInfoData str;
	initStringInfo(&str);

	AppendCreateForeignServerStmt(&str, stmt);

	return str.data;
}


char *
DeparseDropForeignServerStmt(Node *node)
{
	DropStmt *stmt = castNode(DropStmt, node);

	Assert(stmt->removeType == OBJECT_FOREIGN_SERVER);

	StringInfoData str;
	initStringInfo(&str);

	AppendDropForeignServerStmt(&str, stmt);

	return str.data;
}


static void
AppendCreateForeignServerStmt(StringInfo buf, CreateForeignServerStmt *stmt)
{
	appendStringInfoString(buf, "CREATE SERVER ");

	if (stmt->if_not_exists)
	{
		appendStringInfoString(buf, "IF NOT EXISTS ");
	}

	appendStringInfo(buf, "%s ", stmt->servername);

	if (stmt->servertype)
	{
		appendStringInfo(buf, "TYPE %s ", stmt->servertype);
	}

	if (stmt->version)
	{
		appendStringInfo(buf, "VERSION %s ", stmt->version);
	}

	appendStringInfo(buf, "FOREIGN DATA WRAPPER %s ", stmt->fdwname);

	AppendCreateForeignServerOptions(buf, stmt);
}


static void
AppendCreateForeignServerOptions(StringInfo buf, CreateForeignServerStmt *stmt)
{
	if (list_length(stmt->options) <= 0)
	{
		return;
	}

	appendStringInfoString(buf, "OPTIONS (");

	DefElem *def = NULL;
	foreach_ptr(def, stmt->options)
	{
		const char *value = defGetString(def);
		appendStringInfo(buf, "%s \'%s\'", def->defname, value);

		if (def != llast(stmt->options))
		{
			appendStringInfoString(buf, ", ");
		}
	}

	appendStringInfoString(buf, ")");
}


static void
AppendDropForeignServerStmt(StringInfo buf, DropStmt *stmt)
{
	appendStringInfoString(buf, "DROP SERVER ");

	if (stmt->missing_ok)
	{
		appendStringInfoString(buf, "IF EXISTS ");
	}

	AppendServerNames(buf, stmt);

	AppendBehavior(buf, stmt);
}


static void
AppendServerNames(StringInfo buf, DropStmt *stmt)
{
	Value *serverValue = NULL;
	foreach_ptr(serverValue, stmt->objects)
	{
		char *serverString = strVal(serverValue);
		appendStringInfo(buf, "%s", serverString);

		if (serverValue != llast(stmt->objects))
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
