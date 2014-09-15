/* -------------------------------------------------------------------------
 *
 * bdr_apply.c
 *		Replication!!!
 *
 * Replication???
 *
 * Copyright (C) 2012-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/bdr/bdr_apply.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "bdr.h"
#include "bdr_locks.h"

#include "funcapi.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "pgstat.h"

#ifdef BDR_MULTIMASTER
#include "access/committs.h"
#endif
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/xact.h"

#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"

#include "libpq/pqformat.h"

#include "parser/parse_type.h"

#include "replication/logical.h"
#include "bdr_replication_identifier.h"

#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/proc.h"

#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* Useful for development:
#define VERBOSE_INSERT
#define VERBOSE_DELETE
#define VERBOSE_UPDATE
*/

#ifndef BDR_MULTIMASTER
bool bdr_conflict_default_apply = false;
#endif

/* Relation oid cache; initialized then left unchanged */
Oid			QueuedDDLCommandsRelid = InvalidOid;
Oid			QueuedDropsRelid = InvalidOid;

/* Global apply worker state */
uint64		origin_sysid;
TimeLineID	origin_timeline;
Oid			origin_dboid;
bool		started_transaction = false;
/* During apply, holds xid of remote transaction */
TransactionId replication_origin_xid = InvalidTransactionId;

/*
 * For tracking of the remote origin's information when in catchup mode
 * (BDR_OUTPUT_TRANSACTION_HAS_ORIGIN).
 */
static uint64			remote_origin_sysid = 0;
static TimeLineID		remote_origin_timeline_id = 0;
static Oid				remote_origin_dboid = InvalidOid;
static XLogRecPtr		remote_origin_lsn = InvalidXLogRecPtr;
/* The local identifier for the remote's origin, if any. */
static RepNodeId		remote_origin_id = InvalidRepNodeId;

/*
 * This code only runs within an apply bgworker, so we can stash a pointer to our
 * state in shm in a global for convenient access.
 *
 * TODO: make static once bdr_apply_main moved into bdr.c
 */
BdrApplyWorker *bdr_apply_worker = NULL;

/*
 * GUCs for this apply worker - again, this is fixed for the lifetime of the
 * worker so we can stash it in a global.
 */
BdrConnectionConfig *bdr_apply_config = NULL;

dlist_head bdr_lsn_association = DLIST_STATIC_INIT(bdr_lsn_association);

static BDRRelation *read_rel(StringInfo s, LOCKMODE mode);
extern void read_tuple_parts(StringInfo s, BDRRelation *rel, BDRTupleData *tup);

static void check_apply_update(BdrConflictType conflict_type,
							   RepNodeId local_node_id, TimestampTz local_ts,
							   BDRRelation *rel, HeapTuple local_tuple,
							   HeapTuple remote_tuple, HeapTuple *new_tuple,
							   bool *perform_update, bool *log_update,
							   BdrConflictResolution *resolution);

static void do_apply_update(BDRRelation *rel, EState *estate, TupleTableSlot *oldslot,
				TupleTableSlot *newslot);

static void check_sequencer_wakeup(BDRRelation *rel);
#ifdef BDR_MULTIMASTER
static HeapTuple process_queued_drop(HeapTuple cmdtup);
#endif
static void process_queued_ddl_command(HeapTuple cmdtup, bool tx_just_started);
static bool bdr_performing_work(void);

static void process_remote_begin(StringInfo s);
static void process_remote_commit(StringInfo s);
static void process_remote_insert(StringInfo s);
static void process_remote_update(StringInfo s);
static void process_remote_delete(StringInfo s);
#ifdef BDR_MULTIMASTER
static void process_remote_message(StringInfo s);
#endif

static void get_local_tuple_origin(HeapTuple tuple,
								   TimestampTz *commit_ts,
								   RepNodeId *node_id);
static void abs_timestamp_difference(TimestampTz start_time,
									 TimestampTz stop_time,
									 long *secs, int *microsecs);

static void
process_remote_begin(StringInfo s)
{
	XLogRecPtr		origlsn;
	TimestampTz		committime;
	TimestampTz		current;
	TransactionId	remote_xid;
	char			statbuf[100];
	int				apply_delay = bdr_apply_config->apply_delay;
	int				flags = 0;

	Assert(bdr_apply_worker != NULL);

	started_transaction = false;
	remote_origin_id = InvalidRepNodeId;

	flags = pq_getmsgint(s, 4);

	origlsn = pq_getmsgint64(s);
	Assert(origlsn != InvalidXLogRecPtr);
	committime = pq_getmsgint64(s);
	remote_xid = pq_getmsgint(s, 4);

	if (flags & BDR_OUTPUT_TRANSACTION_HAS_ORIGIN)
	{
		remote_origin_sysid = pq_getmsgint64(s);
		remote_origin_timeline_id = pq_getmsgint(s, 4);
		remote_origin_dboid = pq_getmsgint(s, 4);
		remote_origin_lsn = pq_getmsgint64(s);
	}
	else
	{
		/* Transaction originated directly from remote node */
		remote_origin_sysid = 0;
		remote_origin_timeline_id = 0;
		remote_origin_dboid = InvalidOid;
		remote_origin_lsn = InvalidXLogRecPtr;
	}


	/* setup state for commit and conflict detection */
	replication_origin_lsn = origlsn;
	replication_origin_timestamp = committime;

	/* store remote xid for logging and debugging */
	replication_origin_xid = remote_xid;

	snprintf(statbuf, sizeof(statbuf),
			"bdr_apply: BEGIN origin(source, orig_lsn, timestamp): %s, %X/%X, %s",
			 bdr_apply_config->name,
			(uint32) (origlsn >> 32), (uint32) origlsn,
			timestamptz_to_str(committime));

	elog(DEBUG1, "%s", statbuf);

	pgstat_report_activity(STATE_RUNNING, statbuf);

	if (apply_delay == -1)
		apply_delay = bdr_default_apply_delay;

	/*
	 * If we're in catchup mode, see if this transaction is relayed from
	 * elsewhere and advance the appropriate slot.
	 */
	if (flags & BDR_OUTPUT_TRANSACTION_HAS_ORIGIN)
	{
		char remote_ident[256];
		NameData replication_name;

		if (remote_origin_sysid == GetSystemIdentifier()
			&& remote_origin_timeline_id == ThisTimeLineID
			&& remote_origin_dboid == MyDatabaseId)
		{
			/*
			 * This might not have to be an error condition, but we don't cope
			 * with it for now and it shouldn't arise for use of catchup mode
			 * for init_replica.
			 */
			ereport(ERROR,
					(errmsg("Replication loop in catchup mode"),
					 errdetail("Received a transaction from the remote node that originated on this node")));
		}

		/* replication_name is currently unused in bdr */
		NameStr(replication_name)[0] = '\0';

		/*
		 * To determine whether the commit was forwarded by the upstream from
		 * another node, we need to get the local RepNodeId for that node based
		 * on the (sysid, timelineid, dboid) supplied in catchup mode.
		 */
		snprintf(remote_ident, sizeof(remote_ident),
				BDR_NODE_ID_FORMAT,
				remote_origin_sysid, remote_origin_timeline_id, remote_origin_dboid, MyDatabaseId,
				NameStr(replication_name));

		StartTransactionCommand();
		remote_origin_id = GetReplicationIdentifier(remote_ident, false);
		CommitTransactionCommand();
	}

	/* don't want the overhead otherwise */
	if (apply_delay > 0)
	{
		current = GetCurrentIntegerTimestamp();

		/* ensure no weirdness due to clock drift */
		if (current > replication_origin_timestamp)
		{
			long		sec;
			int			usec;

			current = TimestampTzPlusMilliseconds(current,
												  -apply_delay);

			TimestampDifference(current, replication_origin_timestamp,
								&sec, &usec);
			/* FIXME: deal with overflow? */
			pg_usleep(usec + (sec * USECS_PER_SEC));
		}
	}
}

/*
 * Process a commit message from the output plugin, advance replication
 * identifiers, commit the local transaction, and determine whether replay
 * should continue.
 *
 * Returns true if apply should continue with the next record, false if replay
 * should stop after this record.
 */
static void
process_remote_commit(StringInfo s)
{
	XLogRecPtr		commit_lsn;
	TimestampTz		committime;
	TimestampTz		end_lsn;
	int				flags;
#ifndef BDR_MULTIMASTER
	XLogRecPtr XactLastCommitEnd;
#endif

	Assert(bdr_apply_worker != NULL);

	flags = pq_getmsgint(s, 4);

	if (flags != 0)
		elog(ERROR, "Commit flags are currently unused, but flags was set to %i", flags);

	/* order of access to fields after flags is important */
	commit_lsn = pq_getmsgint64(s);
	end_lsn = pq_getmsgint64(s);
	committime = pq_getmsgint64(s);

	elog(DEBUG1, "COMMIT origin(lsn, end, timestamp): %X/%X, %X/%X, %s",
		 (uint32) (commit_lsn >> 32), (uint32) commit_lsn,
		 (uint32) (end_lsn >> 32), (uint32) end_lsn,
		 timestamptz_to_str(committime));

	Assert(commit_lsn == replication_origin_lsn);
	Assert(committime == replication_origin_timestamp);

#ifndef BDR_MULTIMASTER
	XactLastCommitEnd = GetXLogInsertRecPtr();
#endif

	if (started_transaction)
	{
		BdrFlushPosition *flushpos;

		CommitTransactionCommand();

		/*
		 * Associate the end of the remote commit lsn with the local end of
		 * the commit record.
		 */
		flushpos = (BdrFlushPosition *) palloc(sizeof(BdrFlushPosition));
		flushpos->local_end = XactLastCommitEnd;
		flushpos->remote_end = end_lsn;

		dlist_push_tail(&bdr_lsn_association, &flushpos->node);
	}

	pgstat_report_activity(STATE_IDLE, NULL);

	/*
	 * Advance the local replication identifier's lsn, so we don't replay this
	 * commit again.
	 *
	 * We always advance the local replication identifier for the origin node,
	 * even if we're really replaying a commit that's been forwarded from
	 * another node (per remote_origin_id below). This is necessary to make
	 * sure we don't replay the same forwarded commit multiple times.
	 */
	AdvanceCachedReplicationIdentifier(end_lsn, XactLastCommitEnd);

#ifdef BDR_MULTIMASTER
	/*
	 * If we're in catchup mode, see if the commit is relayed from elsewhere
	 * and advance the appropriate slot.
	 */
	if (remote_origin_id != InvalidRepNodeId &&
		remote_origin_id != replication_origin_id)
	{
		/*
		 * The row isn't from the immediate upstream; advance the slot of the
		 * node it originally came from so we start replay of that node's
		 * change data at the right place.
		 */
		AdvanceReplicationIdentifier(remote_origin_id, remote_origin_lsn,
									 XactLastCommitEnd);
	}
#endif

	CurrentResourceOwner = bdr_saved_resowner;

	bdr_count_commit();

	replication_origin_xid = InvalidTransactionId;
	replication_origin_lsn = InvalidXLogRecPtr;
	replication_origin_timestamp = 0;

	/*
	 * Stop replay if we're doing limited replay and we've replayed up to the
	 * last record we're supposed to process.
	 */
	if (bdr_apply_worker->replay_stop_lsn != InvalidXLogRecPtr
			&& bdr_apply_worker->replay_stop_lsn <= end_lsn)
	{
		ereport(LOG,
				(errmsg("bdr apply %s finished processing; replayed to %X/%X of required %X/%X",
				 bdr_apply_config->name,
				 (uint32)(end_lsn>>32), (uint32)end_lsn,
				 (uint32)(bdr_apply_worker->replay_stop_lsn>>32), (uint32)bdr_apply_worker->replay_stop_lsn)));
		/*
		 * We clear the replay_stop_lsn field to indicate successful catchup,
		 * so we don't need a separate flag field in shmem for all apply
		 * workers.
		 */
		bdr_apply_worker->replay_stop_lsn = InvalidXLogRecPtr;

		/* flush all writes so the latest position can be reported back to the sender */
		XLogFlush(GetXLogWriteRecPtr());


		/* Signal that we should stop */
		got_SIGTERM = true;
	}
}

static void
process_remote_insert(StringInfo s)
{
	char		action;
	EState	   *estate;
	BDRTupleData new_tuple;
	TupleTableSlot *newslot;
	TupleTableSlot *oldslot;
	BDRRelation	*rel;
	bool		started_tx;
#ifdef VERBOSE_INSERT
	StringInfoData o;
#endif
	ResultRelInfo *relinfo;
	ItemPointer conflicts;
	bool		conflict = false;
	ScanKey	   *index_keys;
	int			i;
	ItemPointerData conflicting_tid;

	ItemPointerSetInvalid(&conflicting_tid);

	started_tx = bdr_performing_work();

	Assert(bdr_apply_worker != NULL);

	rel = read_rel(s, RowExclusiveLock);

	action = pq_getmsgbyte(s);
	if (action != 'N')
		elog(ERROR, "expected new tuple but got %d",
			 action);

	estate = bdr_create_rel_estate(rel->rel);
	newslot = ExecInitExtraTupleSlot(estate);
	oldslot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(newslot, RelationGetDescr(rel->rel));
	ExecSetSlotDescriptor(oldslot, RelationGetDescr(rel->rel));

	read_tuple_parts(s, rel, &new_tuple);
	{
		HeapTuple tup;
		tup = heap_form_tuple(RelationGetDescr(rel->rel),
							  new_tuple.values, new_tuple.isnull);
		ExecStoreTuple(tup, newslot, InvalidBuffer, true);
	}

	if (rel->rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "unexpected relkind '%c' rel \"%s\"",
			 rel->rel->rd_rel->relkind, RelationGetRelationName(rel->rel));

	/* debug output */
#ifdef VERBOSE_INSERT
	initStringInfo(&o);
	tuple_to_stringinfo(&o, RelationGetDescr(rel), newslot->tts_tuple);
	elog(DEBUG1, "INSERT:%s", o.data);
	resetStringInfo(&o);
#endif


	/*
	 * Search for conflicting tuples.
	 */
	ExecOpenIndices(estate->es_result_relation_info);
	relinfo = estate->es_result_relation_info;
	index_keys = palloc0(relinfo->ri_NumIndices * sizeof(ScanKeyData*));
	conflicts = palloc0(relinfo->ri_NumIndices * sizeof(ItemPointerData));

	build_index_scan_keys(estate, index_keys, &new_tuple);

	/* do a SnapshotDirty search for conflicting tuples */
	for (i = 0; i < relinfo->ri_NumIndices; i++)
	{
		IndexInfo  *ii = relinfo->ri_IndexRelationInfo[i];
		bool found = false;

		/*
		 * Only unique indexes are of interest here, and we can't deal with
		 * expression indexes so far. FIXME: predicates should be handled
		 * better.
		 *
		 * NB: Needs to match expression in build_index_scan_key
		 */
		if (!ii->ii_Unique || ii->ii_Expressions != NIL)
			continue;

		if (index_keys[i] == NULL)
			continue;

		Assert(ii->ii_Expressions == NIL);

		/* if conflict: wait */
		found = find_pkey_tuple(index_keys[i],
								rel, relinfo->ri_IndexRelationDescs[i],
								oldslot, true, LockTupleExclusive);

		/* alert if there's more than one conflicting unique key */
		if (found &&
			ItemPointerIsValid(&conflicting_tid) &&
			!ItemPointerEquals(&oldslot->tts_tuple->t_self,
							   &conflicting_tid))
		{
			/* FIXME: improve logging here */
			elog(ERROR, "diverging uniqueness conflict");
		}
		else if (found)
		{
			ItemPointerCopy(&oldslot->tts_tuple->t_self, &conflicting_tid);
			conflict = true;
			break;
		}
		else
			ItemPointerSetInvalid(&conflicts[i]);

		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * If there's a conflict use the version created later, otherwise do a
	 * plain insert.
	 */
	if (conflict)
	{
		TimestampTz local_ts;
		RepNodeId	local_node_id;
		bool		apply_update;
		bool		log_update;
		BdrApplyConflict *apply_conflict = NULL; /* Mute compiler */
		BdrConflictResolution resolution;

		get_local_tuple_origin(oldslot->tts_tuple, &local_ts, &local_node_id);

		/*
		 * Use conflict triggers and/or last-update-wins to decide which tuple
		 * to retain.
		 */
		check_apply_update(BdrConflictType_InsertInsert,
						   local_node_id, local_ts, rel,
						   oldslot->tts_tuple, NULL, NULL,
						   &apply_update, &log_update, &resolution);

		/*
		 * Log conflict to server log.
		 */
		if (log_update)
		{
			apply_conflict = bdr_make_apply_conflict(
				BdrConflictType_InsertInsert, resolution,
				replication_origin_xid, rel, oldslot, local_node_id,
				newslot, NULL /*no error*/);

			bdr_conflict_log_serverlog(apply_conflict);

			bdr_count_insert_conflict();
		}

		/*
		 * Finally, apply the update.
		 */
		if (apply_update)
		{
			simple_heap_update(rel->rel,
							   &oldslot->tts_tuple->t_self,
							   newslot->tts_tuple);
			/* races will be resolved by abort/retry */
			UserTableUpdateOpenIndexes(estate, newslot);

			bdr_count_insert();
		}

		/* Log conflict to table */
		if (log_update)
		{
			bdr_conflict_log_table(apply_conflict);
			bdr_conflict_logging_cleanup();
		}
	}
	else
	{
		simple_heap_insert(rel->rel, newslot->tts_tuple);
		UserTableUpdateOpenIndexes(estate, newslot);
		bdr_count_insert();
	}

	ExecCloseIndices(estate->es_result_relation_info);

	check_sequencer_wakeup(rel);

	/* execute DDL if insertion was into the ddl command queue */
	if (RelationGetRelid(rel->rel) == QueuedDDLCommandsRelid ||
		RelationGetRelid(rel->rel) == QueuedDropsRelid)
	{
		HeapTuple ht;
		LockRelId	lockid = rel->rel->rd_lockInfo.lockRelId;
		TransactionId oldxid = GetTopTransactionId();
		Oid relid = RelationGetRelid(rel->rel);
		Relation qrel;

		/* there never should be conflicts on these */
		Assert(!conflict);

		/*
		 * Release transaction bound resources for CONCURRENTLY support.
		 */
		MemoryContextSwitchTo(MessageContext);
		ht = heap_copytuple(newslot->tts_tuple);

		LockRelationIdForSession(&lockid, RowExclusiveLock);
		bdr_heap_close(rel, NoLock);

		ExecResetTupleTable(estate->es_tupleTable, true);
		FreeExecutorState(estate);

		if (relid == QueuedDDLCommandsRelid)
			process_queued_ddl_command(ht, started_tx);
		if (relid == QueuedDropsRelid)
#ifdef BDR_MULTIMASTER
			process_queued_drop(ht);
#else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("drop queue is not supported by this build")));
#endif

		qrel = heap_open(QueuedDDLCommandsRelid, RowExclusiveLock);

		UnlockRelationIdForSession(&lockid, RowExclusiveLock);

		heap_close(qrel, NoLock);

		if (oldxid != GetTopTransactionId())
		{
			CommitTransactionCommand();
			started_transaction = false;
		}
	}
	else
	{
		bdr_heap_close(rel, NoLock);
		ExecResetTupleTable(estate->es_tupleTable, true);
		FreeExecutorState(estate);
	}

	CommandCounterIncrement();
}

static void
process_remote_update(StringInfo s)
{
	char		action;
	EState	   *estate;
	TupleTableSlot *newslot;
	TupleTableSlot *oldslot;
	bool		pkey_sent;
	bool		found_tuple;
	BDRTupleData old_tuple;
	BDRTupleData new_tuple;
	Oid			idxoid;
	BDRRelation	*rel;
	Relation	idxrel;
#ifdef VERBOSE_UPDATE
	StringInfoData o;
#endif
	ScanKeyData skey[INDEX_MAX_KEYS];
	HeapTuple	user_tuple = NULL,
				remote_tuple = NULL;

	bdr_performing_work();

	rel = read_rel(s, RowExclusiveLock);

	action = pq_getmsgbyte(s);

	/* old key present, identifying key changed */
	if (action != 'K' && action != 'N')
		elog(ERROR, "expected action 'N' or 'K', got %c",
			 action);

	estate = bdr_create_rel_estate(rel->rel);
	oldslot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(oldslot, RelationGetDescr(rel->rel));
	newslot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(newslot, RelationGetDescr(rel->rel));

	if (action == 'K')
	{
		pkey_sent = true;
		read_tuple_parts(s, rel, &old_tuple);
		action = pq_getmsgbyte(s);
	}
	else
		pkey_sent = false;

	/* check for new  tuple */
	if (action != 'N')
		elog(ERROR, "expected action 'N', got %c",
			 action);

	if (rel->rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "unexpected relkind '%c' rel \"%s\"",
			 rel->rel->rd_rel->relkind, RelationGetRelationName(rel->rel));

	/* read new tuple */
	read_tuple_parts(s, rel, &new_tuple);

	/* lookup index to build scankey */
	if (rel->rel->rd_indexvalid == 0)
		RelationGetIndexList(rel->rel);
	idxoid = rel->rel->rd_replidindex;
	if (!OidIsValid(idxoid))
	{
		elog(ERROR, "could not find primary key for table with oid %u",
			 RelationGetRelid(rel->rel));
		return;
	}

	/* open index, so we can build scan key for row */
	idxrel = index_open(idxoid, RowExclusiveLock);

	Assert(idxrel->rd_index->indisunique);

	/* Use columns from the new tuple if the key didn't change. */
	build_index_scan_key(skey, rel->rel, idxrel,
						 pkey_sent ? &old_tuple : &new_tuple);

	PushActiveSnapshot(GetTransactionSnapshot());

	/* look for tuple identified by the (old) primary key */
	found_tuple = find_pkey_tuple(skey, rel, idxrel, oldslot, true,
						pkey_sent ? LockTupleExclusive : LockTupleNoKeyExclusive);

	if (found_tuple)
	{
		TimestampTz local_ts;
		RepNodeId	local_node_id;
		bool		apply_update;
		bool		log_update;
		BdrApplyConflict *apply_conflict = NULL; /* Mute compiler */
		BdrConflictResolution resolution;

		remote_tuple = heap_modify_tuple(oldslot->tts_tuple,
										 RelationGetDescr(rel->rel),
										 new_tuple.values,
										 new_tuple.isnull,
										 new_tuple.changed);

		ExecStoreTuple(remote_tuple, newslot, InvalidBuffer, true);

#ifdef VERBOSE_UPDATE
		initStringInfo(&o);
		tuple_to_stringinfo(&o, RelationGetDescr(rel->rel), oldslot->tts_tuple);
		appendStringInfo(&o, " to");
		tuple_to_stringinfo(&o, RelationGetDescr(rel->rel), remote_tuple);
		elog(DEBUG1, "UPDATE:%s", o.data);
		resetStringInfo(&o);
#endif

		get_local_tuple_origin(oldslot->tts_tuple, &local_ts, &local_node_id);

		/*
		 * Use conflict triggers and/or last-update-wins to decide which tuple
		 * to retain.
		 */
		check_apply_update(BdrConflictType_UpdateUpdate,
						   local_node_id, local_ts, rel,
						   oldslot->tts_tuple, newslot->tts_tuple,
						   &user_tuple, &apply_update,
						   &log_update, &resolution);

		/*
		 * Log conflict to both server log and table.
		 */
		if (log_update)
		{
			apply_conflict = bdr_make_apply_conflict(
				BdrConflictType_UpdateUpdate, resolution,
				replication_origin_xid, rel, oldslot, local_node_id,
				newslot, NULL /*no error*/);

			bdr_conflict_log_serverlog(apply_conflict);

			bdr_count_update_conflict();
		}

		if (apply_update)
		{
			/* user provided a new tuple; form it to a bdr tuple */
			if (user_tuple != NULL)
			{
#ifdef VERBOSE_UPDATE
				initStringInfo(&o);
				tuple_to_stringinfo(&o, RelationGetDescr(rel->rel), user_tuple);
				elog(DEBUG1, "USER tuple:%s", o.data);
				resetStringInfo(&o);
#endif

				ExecStoreTuple(user_tuple, newslot, InvalidBuffer, true);
			}

			do_apply_update(rel, estate, oldslot, newslot);
		}

		/* Log conflict to table */
		if (log_update)
		{
			bdr_conflict_log_table(apply_conflict);
			bdr_conflict_logging_cleanup();
		}
	}
	else
	{
		/*
		 * Update target is missing. We don't know if this is an update-vs-delete
		 * conflict or if the target tuple came from some 3rd node and hasn't yet
		 * been applied to the local node.
		 */

		bool skip = false;
		BdrApplyConflict *apply_conflict;
		BdrConflictResolution resolution;

		remote_tuple = heap_form_tuple(RelationGetDescr(rel->rel),
									   new_tuple.values,
									   new_tuple.isnull);

		ExecStoreTuple(remote_tuple, newslot, InvalidBuffer, true);

		/* FIXME: only use the slot, not remote_tuple henceforth */
		user_tuple = bdr_conflict_handlers_resolve(rel, NULL,
												   remote_tuple, "UPDATE",
												   BdrConflictType_UpdateDelete,
												   0, &skip);

		bdr_count_update_conflict();

		/* XXX: handle user_tuple */
		if (user_tuple)
			ereport(ERROR,
					(errmsg("UPDATE vs DELETE handler returned a row which"
							" isn't allowed for now")));

		if (skip)
			resolution = BdrConflictResolution_ConflictTriggerSkipChange;
		else if (user_tuple)
			resolution = BdrConflictResolution_ConflictTriggerReturnedTuple;
		else
			resolution = BdrConflictResolution_DefaultSkipChange;

		apply_conflict = bdr_make_apply_conflict(
			BdrConflictType_UpdateDelete, resolution, replication_origin_xid,
			rel, NULL, InvalidRepNodeId, newslot, NULL /*no error*/);

		bdr_conflict_log_serverlog(apply_conflict);
		bdr_conflict_log_table(apply_conflict);
		bdr_conflict_logging_cleanup();
	}

	PopActiveSnapshot();

	check_sequencer_wakeup(rel);

	/* release locks upon commit */
	index_close(idxrel, NoLock);
	bdr_heap_close(rel, NoLock);

	ExecResetTupleTable(estate->es_tupleTable, true);
	FreeExecutorState(estate);

	CommandCounterIncrement();
}

static void
process_remote_delete(StringInfo s)
{
#ifdef VERBOSE_DELETE
	StringInfoData o;
#endif
	char		action;
	EState	   *estate;
	BDRTupleData oldtup;
	TupleTableSlot *oldslot;
	Oid			idxoid;
	BDRRelation	*rel;
	Relation	idxrel;
	ScanKeyData skey[INDEX_MAX_KEYS];
	bool		found_old;

	Assert(bdr_apply_worker != NULL);

	bdr_performing_work();

	rel = read_rel(s, RowExclusiveLock);

	action = pq_getmsgbyte(s);

	if (action != 'K' && action != 'E')
		elog(ERROR, "expected action K or E got %c", action);

	if (action == 'E')
	{
		elog(WARNING, "got delete without pkey");
		bdr_heap_close(rel, NoLock);
		return;
	}

	estate = bdr_create_rel_estate(rel->rel);
	oldslot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(oldslot, RelationGetDescr(rel->rel));

	read_tuple_parts(s, rel, &oldtup);

	/* lookup index to build scankey */
	if (rel->rel->rd_indexvalid == 0)
		RelationGetIndexList(rel->rel);
	idxoid = rel->rel->rd_replidindex;
	if (!OidIsValid(idxoid))
	{
		elog(ERROR, "could not find primary key for table with oid %u",
			 RelationGetRelid(rel->rel));
		return;
	}

	/* Now open the primary key index */
	idxrel = index_open(idxoid, RowExclusiveLock);

	if (rel->rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "unexpected relkind '%c' rel \"%s\"",
			 rel->rel->rd_rel->relkind, RelationGetRelationName(rel->rel));

#ifdef VERBOSE_DELETE
	{
		HeapTuple tup;
		tup = heap_form_tuple(RelationGetDescr(rel->rel),
							  oldtup.values, oldtup.isnull);
		ExecStoreTuple(tup, oldslot, InvalidBuffer, true);
	}
	initStringInfo(&o);
	tuple_to_stringinfo(&o, RelationGetDescr(idxrel), oldslot->tts_tuple);
	elog(DEBUG1, "DELETE old-key:%s", o.data);
	resetStringInfo(&o);
#endif

	PushActiveSnapshot(GetTransactionSnapshot());

	build_index_scan_key(skey, rel->rel, idxrel, &oldtup);

	/* try to find tuple via a (candidate|primary) key */
	found_old = find_pkey_tuple(skey, rel, idxrel, oldslot, true, LockTupleExclusive);

	if (found_old)
	{
		simple_heap_delete(rel->rel, &oldslot->tts_tuple->t_self);
		bdr_count_delete();
	}
	else
	{
		/*
		 * The tuple to be deleted could not be found. This could be a replay
		 * order issue, where node A created a tuple then node B deleted it,
		 * and we've received the changes from node B before the changes from
		 * node A.
		 *
		 * Or it could be a conflict where two nodes deleted the same tuple.
		 * We can't tell the difference. We also can't afford to ignore the
		 * delete in case it is just an ordering issue.
		 *
		 * (This can also arise with an UPDATE that changes the PRIMARY KEY,
		 * as that's effectively a DELETE + INSERT).
		 */

		BdrApplyConflict *apply_conflict;

		bdr_count_delete_conflict();

		/* Since the local tuple is missing, fill slot from the received data. */
		{
			HeapTuple tup;
			tup = heap_form_tuple(RelationGetDescr(rel->rel),
								  oldtup.values, oldtup.isnull);
			ExecStoreTuple(tup, oldslot, InvalidBuffer, true);
		}

		apply_conflict = bdr_make_apply_conflict(
			BdrConflictType_DeleteDelete,
			BdrConflictResolution_DefaultSkipChange, replication_origin_xid,
			rel, NULL, InvalidRepNodeId, oldslot, NULL /*no error*/);

		bdr_conflict_log_serverlog(apply_conflict);
		bdr_conflict_log_table(apply_conflict);
		bdr_conflict_logging_cleanup();
	}

	PopActiveSnapshot();

	check_sequencer_wakeup(rel);

	index_close(idxrel, NoLock);
	bdr_heap_close(rel, NoLock);

	ExecResetTupleTable(estate->es_tupleTable, true);
	FreeExecutorState(estate);

	CommandCounterIncrement();
}

/*
 * Get commit timestamp and origin of the tuple
 */
static void
get_local_tuple_origin(HeapTuple tuple, TimestampTz *commit_ts, RepNodeId *node_id)
{
#ifdef BDR_MULTIMASTER
	TransactionId	xmin;
	CommitExtraData	node_id_raw;

	/* refetch tuple, check for old commit ts & origin */
	xmin = HeapTupleHeaderGetXmin(tuple->t_data);

	TransactionIdGetCommitTsData(xmin, commit_ts, &node_id_raw);
	*node_id = node_id_raw;
#else
	TIMESTAMP_NOBEGIN(*commit_ts);
	*node_id = InvalidRepNodeId;
#endif
}

#ifdef BDR_MULTIMASTER
/*
 * Last update wins conflict handling.
 */
static void
bdr_conflict_last_update_wins(RepNodeId local_node_id,
							  RepNodeId remote_node_id,
							  TimestampTz local_ts,
							  TimestampTz remote_ts,
							  bool *perform_update, bool *log_update,
							  BdrConflictResolution *resolution)
{
	int			cmp;

	cmp = timestamptz_cmp_internal(remote_ts, local_ts);
	if (cmp > 0)
	{
		/* The most recent update is the remote one; apply it */
		*perform_update = true;
		*resolution = BdrConflictResolution_LastUpdateWins_KeepRemote;
		return;
	}
	else if (cmp < 0)
	{
		/* The most recent update is the local one; retain it */
		*log_update = true;
		*perform_update = false;
		*resolution = BdrConflictResolution_LastUpdateWins_KeepLocal;
		return;
	}
	else
	{
		uint64		local_sysid,
					remote_origin_sysid;
		TimeLineID	local_tli,
					remote_tli;
		Oid			local_dboid,
					remote_origin_dboid;
		/*
		 * Timestamps are equal. Use sysid + timeline id to decide which
		 * tuple to retain.
		 */
		bdr_fetch_sysid_via_node_id(local_node_id,
									&local_sysid, &local_tli,
									&local_dboid);
		bdr_fetch_sysid_via_node_id(remote_node_id,
									&remote_origin_sysid, &remote_tli,
									&remote_origin_dboid);

		/*
		 * As the timestamps were equal, we have to break the tie in a
		 * consistent manner that'll match across all nodes.
		 *
		 * Use the ordering of the node's unique identifier, the tuple of
		 * (sysid, timelineid, dboid).
		 */
		if (local_sysid < remote_origin_sysid)
			*perform_update = true;
		else if (local_sysid > remote_origin_sysid)
			*perform_update = false;
		else if (local_tli < remote_tli)
			*perform_update = true;
		else if (local_tli > remote_tli)
			*perform_update = false;
		else if (local_dboid < remote_origin_dboid)
			*perform_update = true;
		else if (local_dboid > remote_origin_dboid)
			*perform_update = false;
		else
			/* shouldn't happen */
			elog(ERROR, "unsuccessful node comparison");

		/*
		 * We don't log whether we used timestamp, sysid or timeline id to
		 * decide which tuple to retain. That'll be in the log record
		 * anyway, so we can reconstruct the decision from the log record
		 * later.
		 */
		if (*perform_update)
		{
			*resolution = BdrConflictResolution_LastUpdateWins_KeepRemote;
		}
		else
		{
			*resolution = BdrConflictResolution_LastUpdateWins_KeepLocal;
			*log_update = true;
		}
	}
}

#else

static void
bdr_conflict_default_apply_resolve(bool *perform_update, bool *log_update,
						   BdrConflictResolution *resolution)
{
	*perform_update = bdr_conflict_default_apply;
	/* For UDR conflicts are never expected so they should always be logged. */
	*log_update = true;
	*resolution = bdr_conflict_default_apply ?
						BdrConflictResolution_DefaultApplyChange :
						BdrConflictResolution_DefaultSkipChange;
}
#endif //BDR_MULTIMASTER

/*
 * Check whether a remote insert or update conflicts with the local row
 * version.
 *
 * User-defined conflict triggers get invoked here.
 *
 * perform_update, log_update is set to true if the update should be performed
 * and logged respectively
 *
 * resolution is set to indicate how the conflict was resolved if log_update
 * is true. Its value is undefined if log_update is false.
 */
static void
check_apply_update(BdrConflictType conflict_type,
				   RepNodeId local_node_id, TimestampTz local_ts,
				   BDRRelation *rel, HeapTuple local_tuple,
				   HeapTuple remote_tuple, HeapTuple *new_tuple,
				   bool *perform_update, bool *log_update,
				   BdrConflictResolution *resolution)
{
	int			microsecs;
	long		secs;

	bool		skip = false;

	/*
	 * ensure that new_tuple is initialized with NULL; there are cases where
	 * we wouldn't touch it otherwise.
	 */
	if (new_tuple)
		*new_tuple = NULL;

	*log_update = false;

	if (local_node_id == replication_origin_id)
	{
		/*
		 * If the row got updated twice within a single node, just apply the
		 * update with no conflict.  Don't warn/log either, regardless of the
		 * timing; that's just too common and valid since normal row level
		 * locking guarantees are met.
		 */
		*perform_update = true;
		return;
	}

	/*
	 * Decide whether to keep the remote or local tuple based on a conflict
	 * trigger (if defined) or last-update-wins.
	 *
	 * If the caller doesn't provide storage for the conflict handler to
	 * store a new tuple in, don't fire any conflict triggers.
	 */

	if (new_tuple)
	{
		/*
		 * --------------
		 * Conflict trigger conflict handling - let the user decide whether to:
		 * - Ignore the remote update;
		 * - Supply a new tuple to replace the current tuple; or
		 * - Take no action and fall through to the next handling option
		 * --------------
		 */

		abs_timestamp_difference(replication_origin_timestamp, local_ts,
								 &secs, &microsecs);

		*new_tuple = bdr_conflict_handlers_resolve(rel, local_tuple, remote_tuple,
												   conflict_type == BdrConflictType_InsertInsert ?
												   "INSERT" : "UPDATE",
												   conflict_type,
												   abs(secs) * 1000000 + abs(microsecs),
												   &skip);

		if (skip)
		{
			*log_update = true;
			*perform_update = false;
			*resolution = BdrConflictResolution_ConflictTriggerSkipChange;
			return;
		}
		else if (*new_tuple)
		{
			/* Custom conflict handler returned tuple, log it. */
			*log_update = true;
			*perform_update = true;
			*resolution = BdrConflictResolution_ConflictTriggerReturnedTuple;
			return;
		}

		/*
		 * if user decided not to skip the conflict but didn't provide a
		 * resolving tuple we fall back to default handling
		 */
	}

#ifdef BDR_MULTIMASTER
	/* Use last update wins conflict handling. */
	bdr_conflict_last_update_wins(local_node_id,
								  replication_origin_id,
								  local_ts,
								  replication_origin_timestamp,
								  perform_update, log_update,
								  resolution);
#else
	bdr_conflict_default_apply_resolve(perform_update, log_update,
									   resolution);
#endif
}

#ifdef BDR_MULTIMASTER
static void
process_remote_message(StringInfo s)
{
	StringInfoData message;
	bool		transactional;
	int			chanlen;
	const char *chan;
	int			type;
	uint64		origin_sysid;
	TimeLineID	origin_tlid;
	Oid			origin_datid;
	int			origin_namelen;
	XLogRecPtr	lsn;

	initStringInfo(&message);

	transactional = pq_getmsgbyte(s);
	lsn = pq_getmsgint64(s);

	message.len = pq_getmsgint(s, 4);
	message.data = (char *) pq_getmsgbytes(s, message.len);

	chanlen = pq_getmsgint(&message, 4);
	chan = pq_getmsgbytes(&message, chanlen);

	if (strncmp(chan, "bdr", chanlen) != 0)
	{
		elog(LOG, "ignoring message in channel %s",
			 pnstrdup(chan, chanlen));
		return;
	}

	type = pq_getmsgint(&message, 4);
	origin_sysid = pq_getmsgint64(&message);
	origin_tlid = pq_getmsgint(&message, 4);
	origin_datid = pq_getmsgint(&message, 4);
	origin_namelen = pq_getmsgint(&message, 4);
	if (origin_namelen != 0)
		elog(ERROR, "no names expected yet");

	elog(DEBUG1, "message type %d from "UINT64_FORMAT":%u database %u at %X/%X",
		 type, origin_sysid, origin_tlid, origin_datid,
		 (uint32) (lsn >> 32),
		 (uint32) lsn);

	if (type == BDR_MESSAGE_START)
	{
		bdr_locks_process_remote_startup(
			origin_sysid, origin_tlid, origin_datid);
	}
	else if (type == BDR_MESSAGE_ACQUIRE_LOCK)
	{
		bdr_process_acquire_ddl_lock(
			origin_sysid, origin_tlid, origin_datid);
	}
	else if (type == BDR_MESSAGE_RELEASE_LOCK)
	{
		uint64		lock_sysid;
		TimeLineID	lock_tlid;
		Oid			lock_datid;

		lock_sysid = pq_getmsgint64(&message);
		lock_tlid = pq_getmsgint(&message, 4);
		lock_datid = pq_getmsgint(&message, 4);

		bdr_process_release_ddl_lock(
			origin_sysid, origin_tlid, origin_datid,
			lock_sysid, lock_tlid, lock_datid);
	}
	else if (type == BDR_MESSAGE_CONFIRM_LOCK)
	{
		uint64		lock_sysid;
		TimeLineID	lock_tlid;
		Oid			lock_datid;

		lock_sysid = pq_getmsgint64(&message);
		lock_tlid = pq_getmsgint(&message, 4);
		lock_datid = pq_getmsgint(&message, 4);

		bdr_process_confirm_ddl_lock(
			origin_sysid, origin_tlid, origin_datid,
			lock_sysid, lock_tlid, lock_datid);
	}
	else if (type == BDR_MESSAGE_DECLINE_LOCK)
	{
		uint64		lock_sysid;
		TimeLineID	lock_tlid;
		Oid			lock_datid;

		lock_sysid = pq_getmsgint64(&message);
		lock_tlid = pq_getmsgint(&message, 4);
		lock_datid = pq_getmsgint(&message, 4);

		bdr_process_decline_ddl_lock(
			origin_sysid, origin_tlid, origin_datid,
			lock_sysid, lock_tlid, lock_datid);
	}
	else if (type == BDR_MESSAGE_REQUEST_REPLAY_CONFIRM)
	{
		XLogRecPtr confirm_lsn;
		confirm_lsn = pq_getmsgint64(&message);

		bdr_process_request_replay_confirm(
			origin_sysid, origin_tlid, origin_datid, confirm_lsn);
	}
	else if (type == BDR_MESSAGE_REPLAY_CONFIRM)
	{
		XLogRecPtr confirm_lsn;
		confirm_lsn = pq_getmsgint64(&message);

		bdr_process_replay_confirm(
			origin_sysid, origin_tlid, origin_datid, confirm_lsn);
	}
	else
		elog(LOG, "unknown message type %d", type);

	if (!transactional)
		AdvanceCachedReplicationIdentifier(lsn, InvalidXLogRecPtr);
}
#endif // BDR_MULTIMASTER

static void
do_apply_update(BDRRelation *rel, EState *estate, TupleTableSlot *oldslot,
				TupleTableSlot *newslot)
{
	simple_heap_update(rel->rel, &oldslot->tts_tuple->t_self, newslot->tts_tuple);
	UserTableUpdateIndexes(estate, newslot);
	bdr_count_update();
}

static void
queued_command_error_callback(void *arg)
{
	errcontext("during DDL replay of ddl statement: %s", (char *) arg);
}

static void
process_queued_ddl_command(HeapTuple cmdtup, bool tx_just_started)
{
	Relation	cmdsrel;
	Datum		datum;
	char	   *command_tag;
	char	   *cmdstr;
	bool		isnull;
	char       *perpetrator;
	List	   *commands;
	ListCell   *command_i;
	bool		isTopLevel;
	MemoryContext oldcontext;
	ErrorContextCallback errcallback;

	/* ----
	 * We can't use spi here, because it implicitly assumes a transaction
	 * context. As we want to be able to replicate CONCURRENTLY commands,
	 * that's not going to work...
	 * So instead do all the work manually, being careful about managing the
	 * lifecycle of objects.
	 * ----
	 */
	oldcontext = MemoryContextSwitchTo(MessageContext);

	cmdsrel = heap_open(QueuedDDLCommandsRelid, NoLock);

	/* fetch the perpetrator user identifier */
	datum = heap_getattr(cmdtup, 3,
						 RelationGetDescr(cmdsrel),
						 &isnull);
	if (isnull)
		elog(ERROR, "null command perpetrator in command tuple in \"%s\"",
			 RelationGetRelationName(cmdsrel));
	perpetrator = TextDatumGetCString(datum);

	/* fetch the command tag */
	datum = heap_getattr(cmdtup, 4,
						 RelationGetDescr(cmdsrel),
						 &isnull);
	if (isnull)
		elog(ERROR, "null command tag in command tuple in \"%s\"",
			 RelationGetRelationName(cmdsrel));
	command_tag = TextDatumGetCString(datum);

	/* finally fetch and execute the command */
	datum = heap_getattr(cmdtup, 5,
						 RelationGetDescr(cmdsrel),
						 &isnull);
	if (isnull)
		elog(ERROR, "null command for \"%s\" command tuple", command_tag);

	cmdstr = TextDatumGetCString(datum);

	/* close relation, command execution might end/start xact */
	heap_close(cmdsrel, NoLock);

	errcallback.callback = queued_command_error_callback;
	errcallback.arg = cmdstr;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	commands = pg_parse_query(cmdstr);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Do a limited amount of safety checking against CONCURRENTLY commands
	 * executed in situations where they aren't allowed. The sender side should
	 * provide protection, but better be safe than sorry.
	 */
	isTopLevel = (list_length(commands) == 1) && tx_just_started;

	foreach(command_i, commands)
	{
		List	   *plantree_list;
		List	   *querytree_list;
		Node	   *command = (Node *) lfirst(command_i);
		const char *commandTag;
		Portal		portal;
		DestReceiver *receiver;

		/* temporarily push snapshot for parse analysis/planning */
		PushActiveSnapshot(GetTransactionSnapshot());

		oldcontext = MemoryContextSwitchTo(MessageContext);

		/*
		 * Set the current role to the user that executed the command on the
		 * origin server.  NB: there is no need to reset this afterwards, as
		 * the value will be gone with our transaction.
		 */
		SetConfigOption("role", perpetrator, PGC_INTERNAL, PGC_S_OVERRIDE);

		commandTag = CreateCommandTag(command);

		querytree_list = pg_analyze_and_rewrite(
			command, cmdstr, NULL, 0);

		plantree_list = pg_plan_queries(
			querytree_list, 0, NULL);

		PopActiveSnapshot();

		portal = CreatePortal("", true, true);
		PortalDefineQuery(portal, NULL,
						  cmdstr, commandTag,
						  plantree_list, NULL);
		PortalStart(portal, NULL, 0, InvalidSnapshot);

		receiver = CreateDestReceiver(DestNone);

		(void) PortalRun(portal, FETCH_ALL,
						 isTopLevel,
						 receiver, receiver,
						 NULL);
		(*receiver->rDestroy) (receiver);

		PortalDrop(portal, false);

		CommandCounterIncrement();

		MemoryContextSwitchTo(oldcontext);
	}

	/* protect against stack resets during CONCURRENTLY processing */
	if (error_context_stack == &errcallback)
		error_context_stack = errcallback.previous;
}


#ifdef BDR_MULTIMASTER
/*
 * ugly hack: Copied struct from dependency.c - there doesn't seem to be a
 * supported way of iterating ObjectAddresses otherwise.
 */
struct ObjectAddresses
{
	ObjectAddress *refs;		/* => palloc'd array */
	void	   *extras;			/* => palloc'd array, or NULL if not used */
	int			numrefs;		/* current number of references */
	int			maxrefs;		/* current size of palloc'd array(s) */
};

static void
queued_drop_error_callback(void *arg)
{
	ObjectAddresses *addrs = (ObjectAddresses *) arg;
	StringInfo s;
	int i;

	s = makeStringInfo();

	for (i = addrs->numrefs - 1; i >= 0; i--)
	{
		ObjectAddress *obj = addrs->refs + i;

		appendStringInfo(s, "\n  * %s", getObjectDescription(obj));
	}
	errcontext("during DDL replay object drop:%s", s->data);
	resetStringInfo(s);
}

static HeapTuple
process_queued_drop(HeapTuple cmdtup)
{
	Relation	cmdsrel;
	HeapTuple	newtup;
	Datum		arrayDatum;
	ArrayType  *array;
	bool		null;
	Oid			elmtype;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Oid			elmoutoid;
	bool		elmisvarlena;
	TupleDesc	elemdesc;
	Datum	   *values;
	int			nelems;
	int			i;
	ObjectAddresses *addresses;
	ErrorContextCallback errcallback;

	cmdsrel = heap_open(QueuedDropsRelid, AccessShareLock);
	arrayDatum = heap_getattr(cmdtup, 3,
							  RelationGetDescr(cmdsrel),
							  &null);
	if (null)
	{
		elog(WARNING, "null dropped object array in command tuple in \"%s\"",
			 RelationGetRelationName(cmdsrel));
		return cmdtup;
	}
	array = DatumGetArrayTypeP(arrayDatum);
	elmtype = ARR_ELEMTYPE(array);

	get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(array, elmtype,
					  elmlen, elmbyval, elmalign,
					  &values, NULL, &nelems);

	getTypeOutputInfo(elmtype, &elmoutoid, &elmisvarlena);
	elemdesc = TypeGetTupleDesc(elmtype, NIL);

	addresses = new_object_addresses();

	for (i = 0; i < nelems; i++)
	{
		HeapTupleHeader	elemhdr;
		HeapTupleData tmptup;
		ObjectType objtype;
		Datum	datum;
		bool	isnull;
		char   *type;
		List   *objnames;
		List   *objargs = NIL;
		Relation objrel;
		ObjectAddress addr;

		elemhdr = (HeapTupleHeader) DatumGetPointer(values[i]);
		tmptup.t_len = HeapTupleHeaderGetDatumLength(elemhdr);
		ItemPointerSetInvalid(&(tmptup.t_self));
		tmptup.t_tableOid = InvalidOid;
		tmptup.t_data = elemhdr;

		/* obtain the object type as a C-string ... */
		datum = heap_getattr(&tmptup, 1, elemdesc, &isnull);
		if (isnull)
		{
			elog(WARNING, "null type !?");
			continue;
		}
		type = TextDatumGetCString(datum);
		objtype = unstringify_objtype(type);

		/*
		 * ignore objects that don't unstringify properly; those are
		 * "internal" objects anyway.
		 */
		if (objtype == -1)
			continue;

		if (objtype == OBJECT_TYPE ||
			objtype == OBJECT_DOMAIN)
		{
			Datum  *values;
			bool   *nulls;
			int		nelems;
			char   *typestring;
			TypeName *typeName;

			datum = heap_getattr(&tmptup, 2, elemdesc, &isnull);
			if (isnull)
			{
				elog(WARNING, "null typename !?");
				continue;
			}

			deconstruct_array(DatumGetArrayTypeP(datum),
							  TEXTOID, -1, false, 'i',
							  &values, &nulls, &nelems);

			typestring = TextDatumGetCString(values[0]);
			typeName = typeStringToTypeName(typestring);
			objnames = typeName->names;
		}
		else if (objtype == OBJECT_FUNCTION ||
				 objtype == OBJECT_AGGREGATE ||
				 objtype == OBJECT_OPERATOR)
		{
			Datum  *values;
			bool   *nulls;
			int		nelems;
			int		i;
			char   *typestring;

			/* objname */
			objnames = NIL;
			datum = heap_getattr(&tmptup, 2, elemdesc, &isnull);
			if (isnull)
			{
				elog(WARNING, "null objname !?");
				continue;
			}

			deconstruct_array(DatumGetArrayTypeP(datum),
							  TEXTOID, -1, false, 'i',
							  &values, &nulls, &nelems);
			for (i = 0; i < nelems; i++)
				objnames = lappend(objnames,
								   makeString(TextDatumGetCString(values[i])));

			/* objargs are type names */
			datum = heap_getattr(&tmptup, 3, elemdesc, &isnull);
			if (isnull)
			{
				elog(WARNING, "null typename !?");
				continue;
			}

			deconstruct_array(DatumGetArrayTypeP(datum),
							  TEXTOID, -1, false, 'i',
							  &values, &nulls, &nelems);

			for (i = 0; i < nelems; i++)
			{
				typestring = TextDatumGetCString(values[i]);
				objargs = lappend(objargs, typeStringToTypeName(typestring));
			}
		}
		else
		{
			Datum  *values;
			bool   *nulls;
			int		nelems;
			int		i;

			/* objname */
			objnames = NIL;
			datum = heap_getattr(&tmptup, 2, elemdesc, &isnull);
			if (isnull)
			{
				elog(WARNING, "null objname !?");
				continue;
			}

			deconstruct_array(DatumGetArrayTypeP(datum),
							  TEXTOID, -1, false, 'i',
							  &values, &nulls, &nelems);
			for (i = 0; i < nelems; i++)
				objnames = lappend(objnames,
								   makeString(TextDatumGetCString(values[i])));

			datum = heap_getattr(&tmptup, 3, elemdesc, &isnull);
			if (!isnull)
			{
				Datum  *values;
				bool   *nulls;
				int		nelems;
				int		i;

				deconstruct_array(DatumGetArrayTypeP(datum),
								  TEXTOID, -1, false, 'i',
								  &values, &nulls, &nelems);
				for (i = 0; i < nelems; i++)
					objargs = lappend(objargs,
									  makeString(TextDatumGetCString(values[i])));
			}
		}

		addr = get_object_address(objtype, objnames, objargs, &objrel,
								  AccessExclusiveLock, false);
		/* unsupported object? */
		if (addr.classId == InvalidOid)
			continue;

		/*
		 * For certain objects, get_object_address returned us an open and
		 * locked relation.  Close it because we have no use for it; but
		 * keeping the lock seems easier than figure out lock level to release.
		 */
		if (objrel != NULL)
			relation_close(objrel, NoLock);

		add_exact_object_address(&addr, addresses);
	}

	errcallback.callback = queued_drop_error_callback;
	errcallback.arg = addresses;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	performMultipleDeletions(addresses, DROP_RESTRICT, 0);

	/* protect against stack resets during CONCURRENTLY processing */
	if (error_context_stack == &errcallback)
		error_context_stack = errcallback.previous;

	newtup = cmdtup;

	heap_close(cmdsrel, AccessShareLock);

	return newtup;
}
#endif // BDR_MULTIMASTER

static bool
bdr_performing_work(void)
{
	if (started_transaction)
	{
		if (CurrentMemoryContext != MessageContext)
			MemoryContextSwitchTo(MessageContext);
		return false;
	}

	started_transaction = true;
	StartTransactionCommand();
	MemoryContextSwitchTo(MessageContext);
	return true;
}

static void
check_sequencer_wakeup(BDRRelation *rel)
{
#ifdef BDR_MULTIMASTER
	Oid			reloid = RelationGetRelid(rel->rel);

	if (reloid == BdrSequenceValuesRelid ||
		reloid == BdrSequenceElectionsRelid ||
		reloid == BdrVotesRelid)
		bdr_schedule_eoxact_sequencer_wakeup();
#endif //BDR_MULTIMASTER
}

void
read_tuple_parts(StringInfo s, BDRRelation *rel, BDRTupleData *tup)
{
	TupleDesc	desc = RelationGetDescr(rel->rel);
	int			i;
	int			rnatts;
	char		action;

	action = pq_getmsgbyte(s);

	if (action != 'T')
		elog(ERROR, "expected TUPLE, got %c", action);

	memset(tup->isnull, 1, sizeof(tup->isnull));
	memset(tup->changed, 1, sizeof(tup->changed));

	rnatts = pq_getmsgint(s, 4);

	if (desc->natts != rnatts)
		elog(ERROR, "tuple natts mismatch, %u vs %u", desc->natts, rnatts);

	/* FIXME: unaligned data accesses */

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = desc->attrs[i];
		char		kind = pq_getmsgbyte(s);
		const char *data;
		int			len;

		switch (kind)
		{
			case 'n': /* null */
				/* already marked as null */
				tup->values[i] = 0xdeadbeef;
				break;
			case 'u': /* unchanged column */
				tup->isnull[i] = true;
				tup->changed[i] = false;
				tup->values[i] = 0xdeadbeef; /* make bad usage more obvious */

				break;

			case 'b': /* binary format */
				tup->isnull[i] = false;
				len = pq_getmsgint(s, 4); /* read length */

				data = pq_getmsgbytes(s, len);

				/* and data */
				if (att->attbyval)
					tup->values[i] = fetch_att(data, true, len);
				else
					tup->values[i] = PointerGetDatum(data);
				break;
			case 's': /* send/recv format */
				{
					Oid typreceive;
					Oid typioparam;
					StringInfoData buf;

					tup->isnull[i] = false;
					len = pq_getmsgint(s, 4); /* read length */

					getTypeBinaryInputInfo(att->atttypid,
										   &typreceive, &typioparam);

					/* create StringInfo pointing into the bigger buffer */
					initStringInfo(&buf);
					/* and data */
					buf.data = (char *) pq_getmsgbytes(s, len);
					buf.len = len;
					tup->values[i] = OidReceiveFunctionCall(
						typreceive, &buf, typioparam, att->atttypmod);

					if (buf.len != buf.cursor)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
								 errmsg("incorrect binary data format")));
					break;
				}
			case 't': /* text format */
				{
					Oid typinput;
					Oid typioparam;

					tup->isnull[i] = false;
					len = pq_getmsgint(s, 4); /* read length */

					getTypeInputInfo(att->atttypid, &typinput, &typioparam);
					/* and data */
					data = (char *) pq_getmsgbytes(s, len);
					tup->values[i] = OidInputFunctionCall(
						typinput, (char *) data, typioparam, att->atttypmod);
				}
				break;
			default:
				elog(ERROR, "unknown column type '%c'", kind);
		}

		if (att->attisdropped && !tup->isnull[i])
			elog(ERROR, "data for dropped column");
	}
}

static BDRRelation *
read_rel(StringInfo s, LOCKMODE mode)
{
	int			relnamelen;
	int			nspnamelen;
	RangeVar*	rv;
	Oid			relid;

	rv = makeNode(RangeVar);

	nspnamelen = pq_getmsgint(s, 2);
	rv->schemaname = (char *) pq_getmsgbytes(s, nspnamelen);

	relnamelen = pq_getmsgint(s, 2);
	rv->relname = (char *) pq_getmsgbytes(s, relnamelen);

	relid = RangeVarGetRelidExtended(rv, mode, false, false, NULL, NULL);

	return bdr_heap_open(relid, NoLock);
}

/*
 * Read a remote action type and process the action record.
 *
 * May set got_SIGTERM to stop processing before next record.
 */
void
bdr_process_remote_action(StringInfo s)
{
	char action = pq_getmsgbyte(s);
	switch (action)
	{
			/* BEGIN */
		case 'B':
			process_remote_begin(s);
			break;
			/* COMMIT */
		case 'C':
			process_remote_commit(s);
			break;
			/* INSERT */
		case 'I':
			process_remote_insert(s);
			break;
			/* UPDATE */
		case 'U':
			process_remote_update(s);
			break;
			/* DELETE */
		case 'D':
			process_remote_delete(s);
			break;
#ifdef BDR_MULTIMASTER
		case 'M':
			process_remote_message(s);
			break;
#endif
		default:
			elog(ERROR, "unknown action of type %c", action);
	}
}


/*
 * Converts an int64 to network byte order.
 */
static void
bdr_sendint64(int64 i, char *buf)
{
	uint32		n32;

	/* High order half first, since we're doing MSB-first */
	n32 = (uint32) (i >> 32);
	n32 = htonl(n32);
	memcpy(&buf[0], &n32, 4);

	/* Now the low order half */
	n32 = (uint32) i;
	n32 = htonl(n32);
	memcpy(&buf[4], &n32, 4);
}

/*
 * Figure out which write/flush positions to report to the walsender process.
 *
 * We can't simply report back the last LSN the walsender sent us because the
 * local transaction might not yet be flushed to disk locally. Instead we
 * build a list that associates local with remote LSNs for every commit. When
 * reporting back the flush position to the sender we iterate that list and
 * check which entries on it are already locally flushed. Those we can report
 * as having been flushed.
 *
 * Returns true if there's no outstanding transactions that need to be
 * flushed.
 */
static bool
bdr_get_flush_position(XLogRecPtr *write, XLogRecPtr *flush)
{
	dlist_mutable_iter iter;
	XLogRecPtr	local_flush = GetFlushRecPtr();

	*write = InvalidXLogRecPtr;
	*flush = InvalidXLogRecPtr;

	dlist_foreach_modify(iter, &bdr_lsn_association)
	{
		BdrFlushPosition *pos =
			dlist_container(BdrFlushPosition, node, iter.cur);

		*write = pos->remote_end;

		if (pos->local_end <= local_flush)
		{
			*flush = pos->remote_end;
			dlist_delete(iter.cur);
			pfree(pos);
		}
		else
		{
			/*
			 * Don't want to uselessly iterate over the rest of the list which
			 * could potentially be long. Instead get the last element and
			 * grab the write position from there.
			 */
			pos = dlist_tail_element(BdrFlushPosition, node,
									 &bdr_lsn_association);
			*write = pos->remote_end;
			return false;
		}
	}

	return dlist_is_empty(&bdr_lsn_association);
}

/*
 * Send a Standby Status Update message to server.
 *
 * 'recvpos' is the latest LSN we've received data to, force is set if we need
 * to send a response to avoid timeouts.
 */
static bool
bdr_send_feedback(PGconn *conn, XLogRecPtr recvpos, int64 now, bool force)
{
	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int			len = 0;

	static XLogRecPtr last_recvpos = InvalidXLogRecPtr;
	static XLogRecPtr last_writepos = InvalidXLogRecPtr;
	static XLogRecPtr last_flushpos = InvalidXLogRecPtr;

	XLogRecPtr writepos;
	XLogRecPtr flushpos;

	/* It's legal to not pass a recvpos */
	if (recvpos < last_recvpos)
		recvpos = last_recvpos;

	if (bdr_get_flush_position(&writepos, &flushpos))
	{
		/*
		 * No outstanding transactions to flush, we can report the latest
		 * received position. This is important for synchronous replication.
		 */
		flushpos = writepos = recvpos;
	}

	if (writepos < last_writepos)
		writepos = last_writepos;

	if (flushpos < last_flushpos)
		flushpos = last_flushpos;

	/* if we've already reported everything we're good */
	if (!force &&
		writepos == last_writepos &&
		flushpos == last_flushpos)
		return true;

	replybuf[len] = 'r';
	len += 1;
	bdr_sendint64(recvpos, &replybuf[len]);			/* write */
	len += 8;
	bdr_sendint64(flushpos, &replybuf[len]);		/* flush */
	len += 8;
	bdr_sendint64(writepos, &replybuf[len]);		/* apply */
	len += 8;
	bdr_sendint64(now, &replybuf[len]);				/* sendTime */
	len += 8;
	replybuf[len] = false;							/* replyRequested */
	len += 1;

	elog(DEBUG2, "sending feedback (force %d) to recv %X/%X, write %X/%X, flush %X/%X",
		 force,
		 (uint32) (recvpos >> 32), (uint32) recvpos,
		 (uint32) (writepos >> 32), (uint32) writepos,
		 (uint32) (flushpos >> 32), (uint32) flushpos
		);


	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send feedback packet: %s",
						PQerrorMessage(conn))));
		return false;
	}

	if (recvpos > last_recvpos)
		last_recvpos = recvpos;
	if (writepos > last_writepos)
		last_writepos = writepos;
	if (flushpos > last_flushpos)
		last_flushpos = flushpos;

	return true;
}

/*
 * abs_timestamp_difference -- convert the difference between two timestamps
 *		into integer seconds and microseconds
 *
 * The result is always the absolute (pozitive difference), so the order
 * of input is not important.
 *
 * If either input is not finite, we return zeroes.
 */
static void
abs_timestamp_difference(TimestampTz start_time, TimestampTz stop_time,
					long *secs, int *microsecs)
{
	if (TIMESTAMP_NOT_FINITE(start_time) || TIMESTAMP_NOT_FINITE(stop_time))
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		TimestampTz diff = abs(stop_time - start_time);
#ifdef HAVE_INT64_TIMESTAMP
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
#else
		*secs = (long) diff;
		*microsecs = (int) ((diff - *secs) * 1000000.0);
#endif
	}
}

/*
 * The actual main loop of a BDR apply worker.
 */
void
bdr_apply_work(PGconn* streamConn)
{
	int			fd;
	char	   *copybuf = NULL;
	XLogRecPtr	last_received = InvalidXLogRecPtr;

	fd = PQsocket(streamConn);

	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	while (!got_SIGTERM)
	{
		/* int		 ret; */
		int			rc;
		int			r;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = WaitLatchOrSocket(&MyProc->procLatch,
							   WL_SOCKET_READABLE | WL_LATCH_SET |
							   WL_TIMEOUT | WL_POSTMASTER_DEATH,
							   fd, 1000L);

		ResetLatch(&MyProc->procLatch);

		MemoryContextSwitchTo(MessageContext);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		if (PQstatus(streamConn) == CONNECTION_BAD)
		{
			bdr_count_disconnect();
			elog(ERROR, "connection to other side has died");
		}

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (rc & WL_SOCKET_READABLE)
			PQconsumeInput(streamConn);

		for (;;)
		{
			if (got_SIGTERM)
				break;

			if (copybuf != NULL)
			{
				PQfreemem(copybuf);
				copybuf = NULL;
			}

			r = PQgetCopyData(streamConn, &copybuf, 1);

			if (r == -1)
			{
				elog(ERROR, "data stream ended");
			}
			else if (r == -2)
			{
				elog(ERROR, "could not read COPY data: %s",
					 PQerrorMessage(streamConn));
			}
			else if (r < 0)
				elog(ERROR, "invalid COPY status %d", r);
			else if (r == 0)
			{
				/* need to wait for new data */
				break;
			}
			else
			{
				int c;
				StringInfoData s;

				MemoryContextSwitchTo(MessageContext);

				initStringInfo(&s);
				s.data = copybuf;
				s.len = r;
				s.maxlen = -1;

				c = pq_getmsgbyte(&s);

				if (c == 'w')
				{
					XLogRecPtr	start_lsn;
					XLogRecPtr	end_lsn;

					start_lsn = pq_getmsgint64(&s);
					end_lsn = pq_getmsgint64(&s);
					pq_getmsgint64(&s); /* sendTime */

					if (last_received < start_lsn)
						last_received = start_lsn;

					if (last_received < end_lsn)
						last_received = end_lsn;

					bdr_process_remote_action(&s);
				}
				else if (c == 'k')
				{
					XLogRecPtr endpos;
					bool reply_requested;

					endpos = pq_getmsgint64(&s);
					/* timestamp = */ pq_getmsgint64(&s);
					reply_requested = pq_getmsgbyte(&s);

					bdr_send_feedback(streamConn, endpos,
									  GetCurrentTimestamp(),
									  reply_requested);
				}
				/* other message types are purposefully ignored */
			}

		}

		/* confirm all writes at once */
		bdr_send_feedback(streamConn, last_received,
						  GetCurrentTimestamp(), false);

		/*
		 * If the user has paused replication with bdr_apply_pause(), we
		 * wait on our procLatch until pg_bdr_apply_resume() unsets the
		 * flag in shmem. We don't pause until the end of the current
		 * transaction, to avoid sleeping with locks held.
		 *
		 * XXX With the 1s timeout below, we don't risk delaying the
		 * resumption too much. But it would be better to use a global
		 * latch that can be set by pg_bdr_apply_resume(), and not have
		 * to wake up so often.
		 */

		while (BdrWorkerCtl->pause_apply && !IsTransactionState())
		{
			ResetLatch(&MyProc->procLatch);
			rc = WaitLatch(&MyProc->procLatch, WL_TIMEOUT, 1000L);
		}
		MemoryContextResetAndDeleteChildren(MessageContext);
	}
}