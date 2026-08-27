#include "postgres.h"

#include <float.h>

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "fmgr.h"
#include "ivfflat.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Find the list that minimizes the distance function
 */
static void
FindInsertPage(Relation index, Datum *values, BlockNumber *insertPage, ListInfo * listInfo)
{
	double		minDistance = DBL_MAX;
	BlockNumber nextblkno = IVFFLAT_HEAD_BLKNO;
	FmgrInfo   *procinfo;
	Oid			collation;

	/* Avoid compiler warning */
	listInfo->blkno = nextblkno;
	listInfo->offno = FirstOffsetNumber;

	procinfo = index_getprocinfo(index, 1, IVFFLAT_DISTANCE_PROC);
	collation = index->rd_indcollation[0];

	/* Search all list pages */
	while (BlockNumberIsValid(nextblkno))
	{
		Buffer		cbuf;
		Page		cpage;
		OffsetNumber maxoffno;

		cbuf = ReadBuffer(index, nextblkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);
		maxoffno = PageGetMaxOffsetNumber(cpage);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			IvfflatList list;
			double		distance;

			list = (IvfflatList) PageGetItem(cpage, PageGetItemId(cpage, offno));
			distance = DatumGetFloat8(FunctionCall2Coll(procinfo, collation, values[0], PointerGetDatum(&list->center)));

			if (distance < minDistance || !BlockNumberIsValid(*insertPage))
			{
				*insertPage = list->insertPage;
				listInfo->blkno = nextblkno;
				listInfo->offno = offno;
				minDistance = distance;
			}
		}

		nextblkno = IvfflatPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);
	}
}

/*
 * Insert a tuple into the index
 */
static void
InsertTuple(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid)
{
	const		IvfflatTypeInfo *typeInfo = IvfflatGetTypeInfo(index);
	Datum		value;
	FmgrInfo   *normprocinfo;
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	BlockNumber insertPage = InvalidBlockNumber;
	ListInfo	listInfo;
	BlockNumber originalInsertPage;
	int			dimensions;
	int			max_vecs = 0;
	Vector	   *vec = NULL;
	IndexTuple	itup = NULL;
	Size		itemsz = 0;
	bool		is_soa = (IvfflatOptionalProcInfo(index, IVFFLAT_TYPE_INFO_PROC) == NULL);

	/* Detoast once for all calls */
	value = PointerGetDatum(PG_DETOAST_DATUM(values[0]));

	/* Normalize if needed */
	normprocinfo = IvfflatOptionalProcInfo(index, IVFFLAT_NORM_PROC);
	if (normprocinfo != NULL)
	{
		Oid			collation = index->rd_indcollation[0];

		if (!IvfflatCheckNorm(normprocinfo, collation, value))
			return;

		value = IvfflatNormValue(typeInfo, collation, value);
	}

	/* Ensure index is valid */
	IvfflatGetMetaPageInfo(index, NULL, &dimensions);

	/* Find the insert page - sets the page and list info */
	FindInsertPage(index, &value, &insertPage, &listInfo);
	Assert(BlockNumberIsValid(insertPage));
	originalInsertPage = insertPage;

	if (is_soa)
	{
		vec = DatumGetVector(value);
		max_vecs = IvfflatMaxAoSoAVecsPerPage(dimensions);
	}
	else
	{
		itup = index_form_tuple(RelationGetDescr(index), &value, isnull);
		itup->t_tid = *heap_tid;
		itemsz = MAXALIGN(IndexTupleSize(itup));
	}

	/* Find a page to insert the item */
	for (;;)
	{
		buf = ReadBuffer(index, insertPage);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);

		if (PageIsEmpty(page))
			break;

		if (is_soa)
		{
			IvfflatAoSoAChunk old_chunk = (IvfflatAoSoAChunk) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));

			if (old_chunk->count < max_vecs)
				break;
		}
		else
		{
			if (PageGetFreeSpace(page) >= itemsz)
				break;
		}

		insertPage = IvfflatPageGetOpaque(page)->nextblkno;

		if (BlockNumberIsValid(insertPage))
		{
			/* Move to next page */
			GenericXLogAbort(state);
			UnlockReleaseBuffer(buf);
		}
		else
		{
			Buffer		newbuf;
			Page		newpage;

			/* Add a new page */
			LockRelationForExtension(index, ExclusiveLock);
			newbuf = IvfflatNewBuffer(index, MAIN_FORKNUM);
			UnlockRelationForExtension(index, ExclusiveLock);

			/* Init new page */
			newpage = GenericXLogRegisterBuffer(state, newbuf, GENERIC_XLOG_FULL_IMAGE);
			IvfflatInitPage(newbuf, newpage);

			/* Update insert page */
			insertPage = BufferGetBlockNumber(newbuf);

			/* Update previous buffer */
			IvfflatPageGetOpaque(page)->nextblkno = insertPage;

			/* Commit */
			GenericXLogFinish(state);

			/* Unlock previous buffer */
			UnlockReleaseBuffer(buf);

			/* Prepare new buffer */
			state = GenericXLogStart(index);
			buf = newbuf;
			page = GenericXLogRegisterBuffer(state, buf, 0);
			break;
		}
	}

	/* Add to next offset */
	if (is_soa)
	{
		if (PageIsEmpty(page))
		{
			Size		chunksz = IvfflatAoSoAChunkSize(1, dimensions);
			IvfflatAoSoAChunk chunk = (IvfflatAoSoAChunk) palloc0(chunksz);
			ItemPointer tids;
			float	   *aosoa_vals;

			chunk->count = 1;
			chunk->dim = (uint16) dimensions;
			tids = IvfflatAoSoAChunkGetTids(chunk);
			aosoa_vals = IvfflatAoSoAChunkGetValues(chunk);
			tids[0] = *heap_tid;
			for (int d = 0; d < dimensions; d++)
				aosoa_vals[d] = vec->x[d];

			if (PageAddItem(page, (Item) chunk, chunksz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add AoSoA chunk to \"%s\"", RelationGetRelationName(index));
			pfree(chunk);
		}
		else
		{
			IvfflatAoSoAChunk old_chunk = (IvfflatAoSoAChunk) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
			int			old_count = old_chunk->count;
			int			new_count = old_count + 1;
			Size		new_chunksz = IvfflatAoSoAChunkSize(new_count, dimensions);
			IvfflatAoSoAChunk new_chunk = (IvfflatAoSoAChunk) palloc0(new_chunksz);
			ItemPointer old_tids;
			float	   *old_values;
			ItemPointer new_tids;
			float	   *new_values;
			int			old_full = old_count / 8;
			int			old_rem = old_count % 8;
			int			new_full = new_count / 8;
			int			new_rem = new_count % 8;
			float	   *tmp_vecs = (float *) palloc(new_count * dimensions * sizeof(float));

			new_chunk->count = (uint16) new_count;
			new_chunk->dim = (uint16) dimensions;

			old_tids = IvfflatAoSoAChunkGetTids(old_chunk);
			old_values = IvfflatAoSoAChunkGetValues(old_chunk);
			new_tids = IvfflatAoSoAChunkGetTids(new_chunk);
			new_values = IvfflatAoSoAChunkGetValues(new_chunk);

			/* Copy TIDs */
			memcpy(new_tids, old_tids, old_count * sizeof(ItemPointerData));
			new_tids[old_count] = *heap_tid;

			/* Unpack old vectors */
			for (int t = 0; t < old_full; t++)
			{
				const float *tile = old_values + t * (8 * dimensions);
				for (int j = 0; j < 8; j++)
				{
					int vec_idx = t * 8 + j;
					float *dst = tmp_vecs + vec_idx * dimensions;
					for (int d = 0; d < dimensions; d++)
						dst[d] = tile[d * 8 + j];
				}
			}
			if (old_rem > 0)
			{
				const float *rem_tile = old_values + old_full * (8 * dimensions);
				for (int j = 0; j < old_rem; j++)
				{
					int vec_idx = old_full * 8 + j;
					float *dst = tmp_vecs + vec_idx * dimensions;
					for (int d = 0; d < dimensions; d++)
						dst[d] = rem_tile[d * old_rem + j];
				}
			}
			/* Append new vector */
			memcpy(tmp_vecs + old_count * dimensions, vec->x, dimensions * sizeof(float));

			/* Repack into new AoSoA chunk */
			for (int t = 0; t < new_full; t++)
			{
				float *tile = new_values + t * (8 * dimensions);
				for (int j = 0; j < 8; j++)
				{
					int vec_idx = t * 8 + j;
					const float *src = tmp_vecs + vec_idx * dimensions;
					for (int d = 0; d < dimensions; d++)
						tile[d * 8 + j] = src[d];
				}
			}
			if (new_rem > 0)
			{
				float *rem_tile = new_values + new_full * (8 * dimensions);
				for (int j = 0; j < new_rem; j++)
				{
					int vec_idx = new_full * 8 + j;
					const float *src = tmp_vecs + vec_idx * dimensions;
					for (int d = 0; d < dimensions; d++)
						rem_tile[d * new_rem + j] = src[d];
				}
			}
			pfree(tmp_vecs);

			PageIndexTupleDelete(page, FirstOffsetNumber);
			if (PageAddItem(page, (Item) new_chunk, new_chunksz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add expanded AoSoA chunk to \"%s\"", RelationGetRelationName(index));
			pfree(new_chunk);
		}
	}
	else
	{
		if (PageAddItem(page, (Item) itup, itemsz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));
	}

	IvfflatCommitBuffer(buf, state);

	/* Update the insert page */
	if (insertPage != originalInsertPage)
		IvfflatUpdateList(index, listInfo, insertPage, originalInsertPage, InvalidBlockNumber, MAIN_FORKNUM);
}

/*
 * Insert a tuple into the index
 */
bool
ivfflatinsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid,
			  Relation heap, IndexUniqueCheck checkUnique
#if PG_VERSION_NUM >= 140000
			  ,bool indexUnchanged
#endif
			  ,IndexInfo *indexInfo
)
{
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	/* Skip nulls */
	if (isnull[0])
		return false;

	/*
	 * Use memory context since detoast, IvfflatNormValue, and
	 * index_form_tuple can allocate
	 */
	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Ivfflat insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);
	oldCtx = MemoryContextSwitchTo(insertCtx);

	/* Insert tuple */
	InsertTuple(index, values, isnull, heap_tid);

	/* Delete memory context */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}
