/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#ifndef PG_ARROW_TUPTABLE_H
#define PG_ARROW_TUPTABLE_H

#include <postgres.h>
#include <access/attnum.h>
#include <access/htup_details.h>
#include <access/tupdesc.h>
#include <catalog/index.h>
#include <catalog/pg_attribute.h>
#include <executor/tuptable.h>
#include <nodes/bitmapset.h>
#include <storage/block.h>
#include <storage/itemptr.h>
#include <storage/off.h>
#include <utils/builtins.h>
#include <utils/hsearch.h>
#include <utils/palloc.h>

#include "arrow_cache.h"
#include "compression/arrow_c_data_interface.h"

#include <limits.h>

/*
 * An Arrow tuple slot is a meta-slot representing a compressed and columnar
 * relation that stores data in two separate child relations: one for
 * non-compressed data and one for compressed data.
 *
 * The Arrow tuple slot also gives an abstraction for vectorized data in arrow
 * format (in case of compressed reads), where value-by-value reads of
 * compressed data simply reads from the same compressed child slot until it
 * is completely consumed. Thus, when consuming a compressed child tuple, the
 * child is decompressed on the first read, while subsequent reads of values
 * in the same compressed tuple just increments the index into the
 * decompressed arrow array.
 *
 * Since an Arrow slot contains a reference to the whole decompressed arrow
 * array, it is possible to consume all the Arrow slot's values (rows) in one
 * vectorized read.
 *
 * To enable the abstraction of a single slot and relation, two child slots
 * are needed that match the expected slot type (BufferHeapTupletableslot) and
 * tuple descriptor of the corresponding child relations.
 *
 * The LRU list is sorted in reverse order so the head element is the LRU
 * element. This is because there is a dlist_pop_head, but no dlist_pop_tail.
 *
 */
typedef struct ArrowTupleTableSlot
{
	VirtualTupleTableSlot base;
	/* child slot: points to either noncompressed_slot or compressed_slot,
	 * depending on which slot is currently the "active" child */
	TupleTableSlot *child_slot;
	/* non-compressed slot: used when reading from the non-compressed child relation */
	TupleTableSlot *noncompressed_slot;
	/* compressed slot: used when reading from the compressed child relation */
	TupleTableSlot *compressed_slot;
	AttrNumber count_attnum; /* Attribute number of the count metadata in compressed slot */
	uint16 tuple_index;		 /* Index of this particular tuple in the compressed
							  * (columnar data) child tuple. Note that the first
							  * value has index 1. If the index is 0 it means the
							  * child slot points to a non-compressed tuple. */
	uint16 total_row_count;
	ArrowColumnCache arrow_cache;

	/* Decompress only these columns. If no columns are set, all columns will
	 * be decompressed. */
	Bitmapset *referenced_attrs;
	Bitmapset *segmentby_attrs;
	Bitmapset *valid_attrs;	 /* Per-column validity up to "tts_nvalid" */
	int16 *attrs_offset_map; /* Offset number mappings between the
							  * non-compressed and compressed
							  * relation */
} ArrowTupleTableSlot;

extern const TupleTableSlotOps TTSOpsArrowTuple;

extern const int16 *arrow_slot_get_attribute_offset_map(TupleTableSlot *slot);
extern TupleTableSlot *ExecStoreArrowTuple(TupleTableSlot *slot, uint16 tuple_index);

#define TTS_IS_ARROWTUPLE(slot) ((slot)->tts_ops == &TTSOpsArrowTuple)

#define InvalidTupleIndex 0
#define MaxCompressedBlockNumber ((BlockNumber) 0x3FFFFF)

#define BLOCKID_BITS (CHAR_BIT * sizeof(BlockIdData))
#define COMPRESSED_FLAG (1UL << (BLOCKID_BITS - 1))
#define TUPINDEX_BITS (10U)
#define TUPINDEX_MASK (((uint64) 1UL << TUPINDEX_BITS) - 1)

/*
 * The "compressed TID" consists of the bits of the TID for the compressed row
 * shifted to insert the tuple index as the least significant bits of the TID.
 */
static inline void
tid_to_compressed_tid(ItemPointerData *out_tid, const ItemPointerData *in_tid, uint16 tuple_index)
{
	const uint64 encoded_tid = itemptr_encode((ItemPointer) in_tid);
	const uint64 encoded_ctid = (encoded_tid << TUPINDEX_BITS) | tuple_index;

	Assert(tuple_index != InvalidTupleIndex);

	/*
	 * There is a check in tidbitmap that offset is never larger than
	 * MaxHeapTuplesPerPage and we will get an error if we do not handle that,
	 * so we store the remainder of that division in the offset and the rest
	 * in the block number.
	 *
	 * Also, the offset number may not be zero, so we add 1 here to make it
	 * satisfy the conditions. Since the check in tidbitmap.c is an error if
	 * offset is strictly larger than MaxHeapTuplesPerPage this will work
	 * correctly.
	 *
	 * Note that the check in ItemPointerIsValid() is weaker, so we can relax
	 * this condition later if necessary.
	 */
	const BlockNumber blockno = COMPRESSED_FLAG | (encoded_ctid / MaxHeapTuplesPerPage);
	const OffsetNumber offsetno = encoded_ctid % MaxHeapTuplesPerPage + 1;

	ItemPointerSet(out_tid, blockno, offsetno);

	Assert(ItemPointerGetOffsetNumber(out_tid) >= 1 &&
		   ItemPointerGetOffsetNumber(out_tid) <= MaxHeapTuplesPerPage);
}

static inline uint16
compressed_tid_to_tid(ItemPointerData *out_tid, const ItemPointerData *in_tid)
{
	const uint64 encoded_ctid =
		MaxHeapTuplesPerPage * (~COMPRESSED_FLAG & ItemPointerGetBlockNumber(in_tid)) +
		(ItemPointerGetOffsetNumber(in_tid) - 1);
	const int64 encoded_tid = encoded_ctid >> TUPINDEX_BITS;
	const uint16 tuple_index = encoded_ctid & TUPINDEX_MASK;

	itemptr_decode(out_tid, encoded_tid);

	Assert(tuple_index != InvalidTupleIndex);
	Assert(ItemPointerGetOffsetNumber(out_tid) >= 1 &&
		   ItemPointerGetOffsetNumber(out_tid) <= MaxHeapTuplesPerPage);

	return tuple_index;
}

static inline void
compressed_tid_increment_idx(ItemPointerData *tid, uint16 increment)
{
	const OffsetNumber offsetno = ItemPointerGetOffsetNumber(tid);

	if ((offsetno + increment) <= MaxHeapTuplesPerPage)
		ItemPointerSetOffsetNumber(tid, offsetno + increment);
	else
	{
		BlockNumber blockno = ItemPointerGetBlockNumber(tid);
		BlockNumber blockincr = (offsetno - 1 + increment) / MaxHeapTuplesPerPage;
		OffsetNumber offincr = (offsetno - 1 + increment) % MaxHeapTuplesPerPage + 1;
		ItemPointerSet(tid, blockno + blockincr, offincr);
	}
}

static inline bool
is_compressed_tid(const ItemPointerData *itemptr)
{
	return (ItemPointerGetBlockNumber(itemptr) & COMPRESSED_FLAG) != 0;
}

extern TupleTableSlot *arrow_slot_get_compressed_slot(TupleTableSlot *slot,
													  const TupleDesc tupdesc);

static inline TupleTableSlot *
arrow_slot_get_noncompressed_slot(TupleTableSlot *slot)
{
	ArrowTupleTableSlot *aslot = (ArrowTupleTableSlot *) slot;

	Assert(TTS_IS_ARROWTUPLE(slot));
	Assert(aslot->noncompressed_slot);

	return aslot->noncompressed_slot;
}

static inline uint16
arrow_slot_total_row_count(TupleTableSlot *slot)
{
	ArrowTupleTableSlot *aslot = (ArrowTupleTableSlot *) slot;

	Assert(TTS_IS_ARROWTUPLE(slot));
	Assert(aslot->total_row_count > 0);

	return aslot->total_row_count;
}

static inline bool
arrow_slot_is_compressed(const TupleTableSlot *slot)
{
	const ArrowTupleTableSlot *aslot = (const ArrowTupleTableSlot *) slot;
	Assert(TTS_IS_ARROWTUPLE(slot));
	return aslot->tuple_index != InvalidTupleIndex;
}

/*
 * Get the row index into the compressed tuple.
 *
 * The index is 1-based (starts at 1).
 * InvalidTupleindex means this is not a compressed tuple.
 */
static inline uint16
arrow_slot_row_index(const TupleTableSlot *slot)
{
	const ArrowTupleTableSlot *aslot = (const ArrowTupleTableSlot *) slot;
	Assert(TTS_IS_ARROWTUPLE(slot));
	return aslot->tuple_index;
}

/*
 * Get the current offset into the arrow array.
 *
 * The offset is 0-based. Returns 0 also for a non-compressed tuple.
 */
static inline uint16
arrow_slot_arrow_offset(const TupleTableSlot *slot)
{
	const ArrowTupleTableSlot *aslot = (const ArrowTupleTableSlot *) slot;
	Assert(TTS_IS_ARROWTUPLE(slot));
	return aslot->tuple_index == InvalidTupleIndex ? 0 : aslot->tuple_index - 1;
}

static inline void
arrow_slot_mark_consumed(TupleTableSlot *slot)
{
	ArrowTupleTableSlot *aslot = (ArrowTupleTableSlot *) slot;

	Assert(TTS_IS_ARROWTUPLE(slot));
	aslot->tuple_index = aslot->total_row_count + 1;
}

static inline bool
arrow_slot_is_consumed(const TupleTableSlot *slot)
{
	const ArrowTupleTableSlot *aslot = (const ArrowTupleTableSlot *) slot;

	Assert(TTS_IS_ARROWTUPLE(slot));

	return TTS_EMPTY(slot) || aslot->tuple_index > aslot->total_row_count;
}

static inline bool
arrow_slot_is_last(const TupleTableSlot *slot)
{
	const ArrowTupleTableSlot *aslot = (const ArrowTupleTableSlot *) slot;

	Assert(TTS_IS_ARROWTUPLE(slot));

	return aslot->tuple_index == aslot->total_row_count;
}

/*
 * Increment an arrow slot to point to a subsequent row.
 *
 * If the slot points to a non-compressed tuple, the incrementation will
 * simply clear the slot.
 *
 * If the slot points to a compressed tuple, the incrementation will
 * clear the slot if it reaches the end of the segment.
 */
static inline TupleTableSlot *
ExecIncrArrowTuple(TupleTableSlot *slot, uint16 increment)
{
	ArrowTupleTableSlot *aslot = (ArrowTupleTableSlot *) slot;

	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	if (unlikely(!TTS_IS_ARROWTUPLE(slot)))
		elog(ERROR, "trying to store an on-disk arrow tuple into wrong type of slot");

	if (aslot->tuple_index == InvalidTupleIndex)
	{
		Assert(aslot->noncompressed_slot);
		ExecClearTuple(slot);
		return slot;
	}

	aslot->tuple_index += increment;

	if (aslot->tuple_index > aslot->total_row_count)
	{
		Assert(aslot->compressed_slot);
		ExecClearTuple(slot);
		return slot;
	}

	if (aslot->tuple_index <= aslot->total_row_count)
		compressed_tid_increment_idx(&slot->tts_tid, increment);

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = 0;

	if (aslot->valid_attrs != NULL)
	{
		pfree(aslot->valid_attrs);
		aslot->valid_attrs = NULL;
	}

	return slot;
}

#define ExecStoreNextArrowTuple(slot) ExecIncrArrowTuple(slot, 1)

extern const int16 *arrow_slot_get_attribute_offset_map(TupleTableSlot *slot);
extern bool is_compressed_col(const TupleDesc tupdesc, AttrNumber attno);
extern const ArrowArray *arrow_slot_get_array(TupleTableSlot *slot, AttrNumber attno);
extern void arrow_slot_set_referenced_attrs(TupleTableSlot *slot, Bitmapset *attrs);

extern Datum tsl_is_compressed_tid(PG_FUNCTION_ARGS);

#endif /* PG_ARROW_TUPTABLE_H */