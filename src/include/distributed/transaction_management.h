/*-------------------------------------------------------------------------
 * transaction_management.h
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef TRANSACTION_MANAGMENT_H
#define TRANSACTION_MANAGMENT_H

#include "lib/ilist.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "lib/stringinfo.h"
#include "nodes/primnodes.h"

/* forward declare, to avoid recursive includes */
struct DistObjectCacheEntry;


/* describes what kind of modifications have occurred in the current transaction */
typedef enum
{
	XACT_MODIFICATION_INVALID = 0, /* placeholder initial value */
	XACT_MODIFICATION_NONE,        /* no modifications have taken place */
	XACT_MODIFICATION_DATA,        /* data modifications (DML) have occurred */
	XACT_MODIFICATION_MULTI_SHARD  /* multi-shard modifications have occurred */
} XactModificationType;


/*
 * Enum defining the state of a coordinated (i.e. a transaction potentially
 * spanning several nodes).
 */
typedef enum CoordinatedTransactionState
{
	/* no coordinated transaction in progress, no connections established */
	COORD_TRANS_NONE,

	/* no coordinated transaction in progress, but connections established */
	COORD_TRANS_IDLE,

	/* coordinated transaction in progress */
	COORD_TRANS_STARTED,

	/* coordinated transaction prepared on all workers */
	COORD_TRANS_PREPARED,

	/* coordinated transaction committed */
	COORD_TRANS_COMMITTED
} CoordinatedTransactionState;


/* Enumeration to keep track of context within nested sub-transactions */
typedef struct SubXactContext
{
	SubTransactionId subId;
	StringInfo setLocalCmds;
} SubXactContext;

/*
 * GUC that determines whether a SELECT in a transaction block should also run in
 * a transaction block on the worker.
 */
extern bool SelectOpensTransactionBlock;

/*
 * GUC that determines whether a function should be considered a transaction
 * block.
 */
extern bool FunctionOpensTransactionBlock;

/* state needed to prevent new connections during modifying transactions */
extern XactModificationType XactModificationLevel;

extern CoordinatedTransactionState CurrentCoordinatedTransactionState;

/* list of connections that are part of the current coordinated transaction */
extern dlist_head InProgressTransactions;

/* controls use of locks to enforce safe commutativity */
extern bool AllModificationsCommutative;

/* we've deprecated this flag, keeping here for some time not to break existing users */
extern bool EnableDeadlockPrevention;

/* number of nested stored procedure call levels we are currently in */
extern int StoredProcedureLevel;

/* number of nested DO block levels we are currently in */
extern int DoBlockLevel;

/* SET LOCAL statements active in the current (sub-)transaction. */
extern StringInfo activeSetStmts;

/* did current transaction modify pg_dist_node? */
extern bool TransactionModifiedNodeMetadata;

/*
 * Coordinated transaction management.
 */
void a_special(char const *caller_name);

extern void UseCoordinatedTransaction(void);
extern bool InCoordinatedTransaction(void);
extern void Use2PCForCoordinatedTransaction(void);
extern bool GetCoordinatedTransactionShouldUse2PC(void);
extern bool IsMultiStatementTransaction(void);
extern void EnsureDistributedTransactionId(void);
extern void EnableInForceDelegatedFuncExecution(Const *distArgument);
extern bool GetInForceDelegatedFuncExecution(void);
extern bool IsShardKeyValueAllowed(Const *shardKey);
extern void ResetAllowedShardKeyValue(void);
extern bool MaybeExecutingUDF(void);


/* initialization function(s) */
extern void InitializeTransactionManagement(void);

/* other functions */
extern List * ActiveSubXactContexts(void);
extern StringInfo BeginAndSetDistributedTransactionIdCommand(void);
extern void TriggerMetadataSyncOnCommit(void);


#endif /*  TRANSACTION_MANAGMENT_H */
