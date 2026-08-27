#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "commands/vacuum.h"
#include "ivfflat.h"
#include "storage/bufmgr.h"
#include "utils/relcache.h"

#if PG_VERSION_NUM >= 180000
#define vacuum_delay_point() vacuum_delay_point(false)
#endif

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *
ivfflatbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	BlockNumber blkno = IVFFLAT_HEAD_BLKNO;
	BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

	if (stats == NULL)
		stats = palloc0_object(IndexBulkDeleteResult);

	/* Iterate over list pages */
	while (BlockNumberIsValid(blkno))
	{
		Buffer		cbuf;
		Page		cpage;
		OffsetNumber coffno;
		OffsetNumber cmaxoffno;
		BlockNumber listPages[MaxOffsetNumber];
		ListInfo	listInfo;

		cbuf = ReadBuffer(index, blkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		cmaxoffno = PageGetMaxOffsetNumber(cpage);

		/* Iterate over lists */
		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			IvfflatList list = (IvfflatList) PageGetItem(cpage, PageGetItemId(cpage, coffno));

			listPages[coffno - FirstOffsetNumber] = list->startPage;
		}

		listInfo.blkno = blkno;
		blkno = IvfflatPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);

		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			BlockNumber searchPage = listPages[coffno - FirstOffsetNumber];
			BlockNumber insertPage = InvalidBlockNumber;

			/* Iterate over entry pages */
			while (BlockNumberIsValid(searchPage))
			{
				Buffer		buf;
				Page		page;
				GenericXLogState *state;
				OffsetNumber offno;
				OffsetNumber maxoffno;
				OffsetNumber deletable[MaxOffsetNumber];
				int			ndeletable;
				bool		is_aosoa = (IvfflatOptionalProcInfo(index, IVFFLAT_TYPE_INFO_PROC) == NULL);

				vacuum_delay_point();

				buf = ReadBufferExtended(index, MAIN_FORKNUM, searchPage, RBM_NORMAL, bas);

				/*
				 * ambulkdelete cannot delete entries from pages that are
				 * pinned by other backends
				 *
				 * https://www.postgresql.org/docs/current/index-locking.html
				 */
				LockBufferForCleanup(buf);

				state = GenericXLogStart(index);
				page = GenericXLogRegisterBuffer(state, buf, 0);

				maxoffno = PageGetMaxOffsetNumber(page);
				ndeletable = 0;

				if (is_aosoa)
				{
					/* Find deleted tuples in AoSoA chunks */
					for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
					{
						IvfflatAoSoAChunk chunk = (IvfflatAoSoAChunk) PageGetItem(page, PageGetItemId(page, offno));
						ItemPointer tids = IvfflatAoSoAChunkGetTids(chunk);
						float	   *values = IvfflatAoSoAChunkGetValues(chunk);
						int			count = chunk->count;
						int			dim = chunk->dim;
						int			kept = 0;
						bool		has_deleted = false;

						for (int j = 0; j < count; j++)
						{
							if (callback(&tids[j], callback_state))
							{
								stats->tuples_removed++;
								has_deleted = true;
							}
							else
							{
								stats->num_index_tuples++;
								kept++;
							}
						}

						if (has_deleted)
						{
							if (kept == 0)
							{
								deletable[ndeletable++] = offno;
							}
							else
							{
								Size		new_chunksz = IvfflatAoSoAChunkSize(kept, dim);
								IvfflatAoSoAChunk new_chunk = (IvfflatAoSoAChunk) palloc0(new_chunksz);
								ItemPointer new_tids;
								float	   *new_values;
								int			old_full = count / 8;
								int			old_rem = count % 8;
								int			new_full = kept / 8;
								int			new_rem = kept % 8;
								float	   *surviving_vecs = (float *) palloc(kept * dim * sizeof(float));
								int			cur = 0;

								new_chunk->count = (uint16) kept;
								new_chunk->dim = (uint16) dim;
								new_tids = IvfflatAoSoAChunkGetTids(new_chunk);
								new_values = IvfflatAoSoAChunkGetValues(new_chunk);

								/* Unpack surviving vectors */
								for (int j = 0; j < count; j++)
								{
									if (!callback(&tids[j], callback_state))
									{
										int tile_idx = j / 8;
										int in_tile = j % 8;
										float *dst = surviving_vecs + cur * dim;

										new_tids[cur] = tids[j];
										if (tile_idx < old_full)
										{
											const float *tile = values + tile_idx * (8 * dim);
											for (int d = 0; d < dim; d++)
												dst[d] = tile[d * 8 + in_tile];
										}
										else
										{
											const float *rem_tile = values + old_full * (8 * dim);
											for (int d = 0; d < dim; d++)
												dst[d] = rem_tile[d * old_rem + in_tile];
										}
										cur++;
									}
								}

								/* Repack surviving vectors into AoSoA */
								for (int t = 0; t < new_full; t++)
								{
									float *tile = new_values + t * (8 * dim);
									for (int j = 0; j < 8; j++)
									{
										int vec_idx = t * 8 + j;
										const float *src = surviving_vecs + vec_idx * dim;
										for (int d = 0; d < dim; d++)
											tile[d * 8 + j] = src[d];
									}
								}
								if (new_rem > 0)
								{
									float *rem_tile = new_values + new_full * (8 * dim);
									for (int j = 0; j < new_rem; j++)
									{
										int vec_idx = new_full * 8 + j;
										const float *src = surviving_vecs + vec_idx * dim;
										for (int d = 0; d < dim; d++)
											rem_tile[d * new_rem + j] = src[d];
									}
								}
								pfree(surviving_vecs);

								PageIndexTupleDelete(page, offno);
								PageAddItem(page, (Item) new_chunk, new_chunksz, offno, false, false);
								pfree(new_chunk);
							}
						}
					}
				}
				else
				{
					for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
					{
						IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offno));
						ItemPointer htup = &(itup->t_tid);

						if (callback(htup, callback_state))
						{
							deletable[ndeletable++] = offno;
							stats->tuples_removed++;
						}
						else
							stats->num_index_tuples++;
					}
				}

				/* Set to first free page */
				/* Must be set before searchPage is updated */
				if (!BlockNumberIsValid(insertPage) && (ndeletable > 0 || stats->tuples_removed > 0))
					insertPage = searchPage;

				searchPage = IvfflatPageGetOpaque(page)->nextblkno;

				if (ndeletable > 0)
				{
					PageIndexMultiDelete(page, deletable, ndeletable);
					GenericXLogFinish(state);
				}
				else if (stats->tuples_removed > 0)
				{
					GenericXLogFinish(state);
				}
				else
					GenericXLogAbort(state);

				UnlockReleaseBuffer(buf);
			}

			/*
			 * Update after all tuples deleted.
			 *
			 * We don't add or delete items from lists pages, so offset won't
			 * change.
			 */
			if (BlockNumberIsValid(insertPage))
			{
				listInfo.offno = coffno;
				IvfflatUpdateList(index, listInfo, insertPage, InvalidBlockNumber, InvalidBlockNumber, MAIN_FORKNUM);
			}
		}
	}

	FreeAccessStrategy(bas);

	return stats;
}

/*
 * Clean up after a VACUUM operation
 */
IndexBulkDeleteResult *
ivfflatvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	rel = info->index;

	if (info->analyze_only)
		return stats;

	/* stats is NULL if ambulkdelete not called */
	/* OK to return NULL if index not changed */
	if (stats == NULL)
		return NULL;

	stats->num_pages = RelationGetNumberOfBlocks(rel);

	return stats;
}
