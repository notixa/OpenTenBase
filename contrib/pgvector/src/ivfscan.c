#include "postgres.h"

#include <float.h>

#include "access/genam.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/tupdesc.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "lib/pairingheap.h"
#include "ivfflat.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#define GetScanList(ptr) pairingheap_container(IvfflatScanList, ph_node, ptr)
#define GetScanListConst(ptr) pairingheap_const_container(IvfflatScanList, ph_node, ptr)

/*
 * Compare list distances
 */
static int
CompareLists(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	if (GetScanListConst(a)->distance > GetScanListConst(b)->distance)
		return 1;

	if (GetScanListConst(a)->distance < GetScanListConst(b)->distance)
		return -1;

	return 0;
}

/*
 * Get lists and sort by distance
 */
static void
GetScanLists(IndexScanDesc scan, Datum value)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	BlockNumber nextblkno = IVFFLAT_HEAD_BLKNO;
	int			listCount = 0;
	double		maxDistance = DBL_MAX;

	/* Search all list pages */
	while (BlockNumberIsValid(nextblkno))
	{
		Buffer		cbuf;
		Page		cpage;
		OffsetNumber maxoffno;

		cbuf = ReadBuffer(scan->indexRelation, nextblkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		maxoffno = PageGetMaxOffsetNumber(cpage);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			IvfflatList list = (IvfflatList) PageGetItem(cpage, PageGetItemId(cpage, offno));
			double		distance;

			/* Use procinfo from the index instead of scan key for performance */
			distance = DatumGetFloat8(so->distfunc(so->procinfo, so->collation, PointerGetDatum(&list->center), value));

			if (listCount < so->maxProbes)
			{
				IvfflatScanList *scanlist;

				scanlist = &so->lists[listCount];
				scanlist->startPage = list->startPage;
				scanlist->distance = distance;
				listCount++;

				/* Add to heap */
				pairingheap_add(so->listQueue, &scanlist->ph_node);

				/* Calculate max distance */
				if (listCount == so->maxProbes)
					maxDistance = GetScanList(pairingheap_first(so->listQueue))->distance;
			}
			else if (distance < maxDistance)
			{
				IvfflatScanList *scanlist;

				/* Remove */
				scanlist = GetScanList(pairingheap_remove_first(so->listQueue));

				/* Reuse */
				scanlist->startPage = list->startPage;
				scanlist->distance = distance;
				pairingheap_add(so->listQueue, &scanlist->ph_node);

				/* Update max distance */
				maxDistance = GetScanList(pairingheap_first(so->listQueue))->distance;
			}
		}

		nextblkno = IvfflatPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);
	}

	for (int i = listCount - 1; i >= 0; i--)
		so->listPages[i] = GetScanList(pairingheap_remove_first(so->listQueue))->startPage;

	Assert(pairingheap_is_empty(so->listQueue));
}

/*
 * Top-K Heap operations
 */
static void
ivfflat_heap_insert(IvfflatScanOpaque so, double distance, ItemPointer heaptid)
{
	if (so->heap_cur_size < so->heap_max_size)
	{
		int i = so->heap_cur_size++;
		while (i > 0)
		{
			int parent = (i - 1) / 2;
			if (so->heap_distances[parent] >= distance)
				break;
			so->heap_distances[i] = so->heap_distances[parent];
			so->heap_tids[i] = so->heap_tids[parent];
			i = parent;
		}
		so->heap_distances[i] = distance;
		so->heap_tids[i] = *heaptid;
	}
	else if (distance < so->heap_distances[0])
	{
		int i = 0;
		int size = so->heap_cur_size;
		while (2 * i + 1 < size)
		{
			int left = 2 * i + 1;
			int right = 2 * i + 2;
			int largest = left;
			if (right < size && so->heap_distances[right] > so->heap_distances[left])
				largest = right;
			if (distance >= so->heap_distances[largest])
				break;
			so->heap_distances[i] = so->heap_distances[largest];
			so->heap_tids[i] = so->heap_tids[largest];
			i = largest;
		}
		so->heap_distances[i] = distance;
		so->heap_tids[i] = *heaptid;
	}
}

static void
ivfflat_heap_sort(IvfflatScanOpaque so)
{
	/* Simple insertion sort or we can just use qsort, but since we are keeping indices mapped, 
	   we need to sort the paired arrays. An array of structs would be easier. 
	   Wait, since we used parallel arrays, qsort requires a custom swap. 
	   Let's just implement a quick bubble sort for the small K, or create a temporary struct array. */
	for (int i = 0; i < so->heap_cur_size - 1; i++) {
		for (int j = 0; j < so->heap_cur_size - i - 1; j++) {
			if (so->heap_distances[j] > so->heap_distances[j + 1]) {
				double tmp_d;
				ItemPointerData tmp_t;
				tmp_d = so->heap_distances[j];
				so->heap_distances[j] = so->heap_distances[j + 1];
				so->heap_distances[j + 1] = tmp_d;
				tmp_t = so->heap_tids[j];
				so->heap_tids[j] = so->heap_tids[j + 1];
				so->heap_tids[j + 1] = tmp_t;
			}
		}
	}
}

/*
 * Get items
 */
static void
GetScanItems(IndexScanDesc scan, Datum value)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	TupleTableSlot *slot = so->vslot;
	int			batchProbes = 0;

	if (so->use_heap) {
		so->heap_cur_size = 0;
		so->heap_returned_idx = 0;
	} else {
		tuplesort_reset(so->sortstate);
	}

	/* Search closest probes lists */
	while (so->listIndex < so->maxProbes && (++batchProbes) <= so->probes)
	{
		BlockNumber searchPage = so->listPages[so->listIndex++];

		/* Search all entry pages for list */
		while (BlockNumberIsValid(searchPage))
		{
			Buffer		buf;
			Page		page;
			OffsetNumber maxoffno;
			bool		is_aosoa = (IvfflatOptionalProcInfo(scan->indexRelation, IVFFLAT_TYPE_INFO_PROC) == NULL);

			buf = ReadBufferExtended(scan->indexRelation, MAIN_FORKNUM, searchPage, RBM_NORMAL, so->bas);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			maxoffno = PageGetMaxOffsetNumber(page);

			if (is_aosoa)
			{
				for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
				{
					ItemId		itemid = PageGetItemId(page, offno);
					IvfflatAoSoAChunk chunk = (IvfflatAoSoAChunk) PageGetItem(page, itemid);
					int			chunk_count = chunk->count;
					int			dim = chunk->dim;
					ItemPointer tids = IvfflatAoSoAChunkGetTids(chunk);
					float	   *values = IvfflatAoSoAChunkGetValues(chunk);
					double		distances[64];
					Vector	   *qvec = (DatumGetPointer(value) != NULL) ? DatumGetVector(value) : NULL;

					/* In-Place Zero-Copy AoSoA SIMD Distance Calculation on the shared buffer page */
					if (so->aosoa_inplace_distfunc != NULL && qvec != NULL)
					{
						so->aosoa_inplace_distfunc(dim, qvec->x, values, distances, chunk_count);
					}
					else
					{
						for (int i = 0; i < chunk_count; i++)
						{
							/* Fallback */
							distances[i] = 0.0;
						}
					}

					if (so->use_heap)
					{
						for (int i = 0; i < chunk_count; i++)
						{
							if (so->heap_cur_size < so->heap_max_size || distances[i] < so->heap_distances[0])
							{
								ivfflat_heap_insert(so, distances[i], &tids[i]);
							}
						}
					}
					else
					{
						for (int i = 0; i < chunk_count; i++)
						{
							ExecClearTuple(slot);
							slot->tts_values[0] = Float8GetDatum(distances[i]);
							slot->tts_isnull[0] = false;
							slot->tts_values[1] = PointerGetDatum(&tids[i]);
							slot->tts_isnull[1] = false;
							ExecStoreVirtualTuple(slot);
							tuplesort_puttupleslot(so->sortstate, slot);
						}
					}
				}
			}
			else
			{
				TupleDesc	tupdesc = RelationGetDescr(scan->indexRelation);

				for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
				{
					IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offno));
					Datum		tupleValue;
					bool		isnull;
					double		dist;

					tupleValue = index_getattr(itup, 1, tupdesc, &isnull);
					dist = DatumGetFloat8(so->distfunc(so->procinfo, so->collation, tupleValue, value));

					if (so->use_heap)
					{
						if (so->heap_cur_size < so->heap_max_size || dist < so->heap_distances[0])
						{
							ivfflat_heap_insert(so, dist, &itup->t_tid);
						}
					}
					else
					{
						ExecClearTuple(slot);
						slot->tts_values[0] = Float8GetDatum(dist);
						slot->tts_isnull[0] = false;
						slot->tts_values[1] = PointerGetDatum(&itup->t_tid);
						slot->tts_isnull[1] = false;
						ExecStoreVirtualTuple(slot);
						tuplesort_puttupleslot(so->sortstate, slot);
					}
				}
			}

			searchPage = IvfflatPageGetOpaque(page)->nextblkno;

			UnlockReleaseBuffer(buf);
		}
	}

	if (so->use_heap) {
		ivfflat_heap_sort(so);
	} else {
		tuplesort_performsort(so->sortstate);
	}

#if defined(IVFFLAT_MEMORY)
	elog(INFO, "memory: %zu MB", MemoryContextMemAllocated(CurrentMemoryContext, true) / (1024 * 1024));
#endif
}

/*
 * Zero distance
 */
static Datum
ZeroDistance(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
	return Float8GetDatum(0.0);
}

/*
 * Get scan value
 */
static Datum
GetScanValue(IndexScanDesc scan)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	Datum		value;

	if (scan->orderByData->sk_flags & SK_ISNULL)
	{
		value = PointerGetDatum(NULL);
		so->distfunc = ZeroDistance;
	}
	else
	{
		value = scan->orderByData->sk_argument;
		so->distfunc = FunctionCall2Coll;

		/* Value should not be compressed or toasted */
		Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));
		Assert(!VARATT_IS_EXTENDED(DatumGetPointer(value)));

		/* Normalize if needed */
		if (so->normprocinfo != NULL)
		{
			MemoryContext oldCtx = MemoryContextSwitchTo(so->tmpCtx);

			value = IvfflatNormValue(so->typeInfo, so->collation, value);

			MemoryContextSwitchTo(oldCtx);
		}
	}

	return value;
}

/*
 * Initialize scan sort state
 */
static Tuplesortstate *
InitScanSortState(TupleDesc tupdesc)
{
	AttrNumber	attNums[] = {1};
	Oid			sortOperators[] = {Float8LessOperator};
	Oid			sortCollations[] = {InvalidOid};
	bool		nullsFirstFlags[] = {false};

	return tuplesort_begin_heap(tupdesc, 1, attNums, sortOperators, sortCollations, nullsFirstFlags, work_mem, NULL, false);
}

/*
 * Prepare for an index scan
 */
IndexScanDesc
ivfflatbeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	IvfflatScanOpaque so;
	int			lists;
	int			dimensions;
	int			probes = ivfflat_probes;
	int			maxProbes;
	MemoryContext oldCtx;

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	/* Get lists and dimensions from metapage */
	IvfflatGetMetaPageInfo(index, &lists, &dimensions);

	if (ivfflat_iterative_scan != IVFFLAT_ITERATIVE_SCAN_OFF)
		maxProbes = Max(ivfflat_max_probes, probes);
	else
		maxProbes = probes;

	if (probes > lists)
		probes = lists;

	if (maxProbes > lists)
		maxProbes = lists;

	so = palloc_object(IvfflatScanOpaqueData);
	so->typeInfo = IvfflatGetTypeInfo(index);
	so->first = true;
	so->probes = probes;
	so->maxProbes = maxProbes;
	so->dimensions = dimensions;
	so->value = PointerGetDatum(NULL);

	/* Set support functions */
	so->procinfo = index_getprocinfo(index, 1, IVFFLAT_DISTANCE_PROC);
	so->normprocinfo = IvfflatOptionalProcInfo(index, IVFFLAT_NORM_PROC);
	so->collation = index->rd_indcollation[0];
	so->aosoa_inplace_distfunc = (VectorAoSoABatchDistFunc_InPlace) VectorGetAoSoABatchDistFunc_InPlace(so->procinfo->fn_addr);

	so->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Ivfflat scan temporary context",
									   ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(so->tmpCtx);

	/* Create tuple description for sorting */
	so->tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(so->tupdesc, (AttrNumber) 1, "distance", FLOAT8OID, -1, 0);
	TupleDescInitEntry(so->tupdesc, (AttrNumber) 2, "heaptid", TIDOID, -1, 0);
#if PG_VERSION_NUM >= 190000
	TupleDescFinalize(so->tupdesc);
#endif

	so->use_heap = (ivfflat_top_k > 0);
	if (so->use_heap) {
		so->heap_max_size = ivfflat_top_k;
		so->heap_cur_size = 0;
		so->heap_returned_idx = 0;
		so->heap_distances = palloc_array_checked(double, so->heap_max_size);
		so->heap_tids = palloc_array_checked(ItemPointerData, so->heap_max_size);
		so->sortstate = NULL; /* Not used */
	} else {
		/* Prep sort */
		so->sortstate = InitScanSortState(so->tupdesc);
	}

	/* Need separate slots for puttuple and gettuple */
	so->vslot = MakeSingleTupleTableSlot(so->tupdesc, &TTSOpsVirtual);
	so->mslot = MakeSingleTupleTableSlot(so->tupdesc, &TTSOpsMinimalTuple);

	/*
	 * Reuse same set of shared buffers for scan
	 *
	 * See postgres/src/backend/storage/buffer/README for description
	 */
	so->bas = GetAccessStrategy(BAS_BULKREAD);

	so->listQueue = pairingheap_allocate(CompareLists, scan);
	so->listPages = palloc_array_checked(BlockNumber, (Size) maxProbes);
	so->listIndex = 0;
	so->lists = palloc_array_checked(IvfflatScanList, (Size) maxProbes);

	MemoryContextSwitchTo(oldCtx);

	scan->opaque = so;

	return scan;
}

/*
 * Start or restart an index scan
 */
void
ivfflatrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;

	so->first = true;
	pairingheap_reset(so->listQueue);
	so->listIndex = 0;

	if (so->normprocinfo != NULL && DatumGetPointer(so->value) != NULL)
	{
		pfree(DatumGetPointer(so->value));
		so->value = PointerGetDatum(NULL);
	}

	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, (Size) scan->numberOfKeys * sizeof(ScanKeyData));

	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys, (Size) scan->numberOfOrderBys * sizeof(ScanKeyData));
}

/*
 * Fetch the next tuple in the given scan
 */
bool
ivfflatgettuple(IndexScanDesc scan, ScanDirection dir)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	ItemPointer heaptid;
	bool		isnull;

	/*
	 * Index can be used to scan backward, but Postgres doesn't support
	 * backward scan on operators
	 */
	Assert(ScanDirectionIsForward(dir));

	if (so->first)
	{
		Datum		value;

		/* Count index scan for stats */
		pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
		if (scan->instrument)
			scan->instrument->nsearches++;
#endif

		/* Safety check */
		if (scan->orderByData == NULL)
			elog(ERROR, "cannot scan ivfflat index without order");

		/* Requires MVCC-compliant snapshot as not able to pin during sorting */
		/* https://www.postgresql.org/docs/current/index-locking.html */
		if (!IsMVCCSnapshot(scan->xs_snapshot))
			elog(ERROR, "non-MVCC snapshots are not supported with ivfflat");

		value = GetScanValue(scan);
		IvfflatBench("GetScanLists", GetScanLists(scan, value));
		IvfflatBench("GetScanItems", GetScanItems(scan, value));
		so->first = false;
		so->value = value;
	}

	if (so->use_heap) {
		if (so->heap_returned_idx >= so->heap_cur_size) {
			if (so->listIndex == so->maxProbes)
				return false;
			/* Technically bounded heap is one-pass. If more probes are added iteratively, 
			   GetScanItems will reset the heap. But with a bound, usually iterative scan is off or we just return what we have. */
			IvfflatBench("GetScanItems", GetScanItems(scan, so->value));
			if (so->heap_returned_idx >= so->heap_cur_size)
				return false;
		}
		scan->xs_heaptid = so->heap_tids[so->heap_returned_idx];
		so->heap_returned_idx++;
	} else {
		while (!tuplesort_gettupleslot(so->sortstate, true, false, so->mslot, NULL))
		{
			if (so->listIndex == so->maxProbes)
				return false;

			IvfflatBench("GetScanItems", GetScanItems(scan, so->value));
		}

		heaptid = (ItemPointer) DatumGetPointer(slot_getattr(so->mslot, 2, &isnull));
		scan->xs_heaptid = *heaptid;
	}

	scan->xs_recheck = false;
	scan->xs_recheckorderby = false;
	return true;
}

/*
 * End a scan and release resources
 */
void
ivfflatendscan(IndexScanDesc scan)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;

	/* Free any temporary files */
	if (so->sortstate != NULL)
		tuplesort_end(so->sortstate);

	MemoryContextDelete(so->tmpCtx);

	pfree(so);
	scan->opaque = NULL;
}
