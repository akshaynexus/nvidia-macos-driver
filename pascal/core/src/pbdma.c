// SPDX-License-Identifier: MIT
/*
 * pascal/pbdma.c - software model of the GP106 PBDMA (see pbdma.h).
 *
 * A straight-line implementation of the "Host Pushbuffer Format"
 * processing described in NVIDIA open-gpu-doc dev_ram.ref.txt
 * ("Host Pushbuffer Format (FIFO_DMA)") and dev_pbdma.ref.txt (GP entry
 * + interrupt conditions), with field positions from the Pascal class
 * header third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h. Both docs are MIT-licensed NVIDIA
 * material; no nouveau code is reproduced here (nouveau's nvkm never
 * decodes pushbuffer methods on the CPU anyway - there is no kernel-side
 * decoder to derive from, which is why this model follows the reference
 * docs field-by-field instead).
 *
 * Processing order per GP entry (dev_pbdma.ref "GP_ENTRY"):
 *   LENGTH == 0  -> control entry (OPCODE NOP/GP_CRC/PB_CRC; ILLEGAL or
 *                   invalid => GPENTRY interrupt, entry discarded, PBDMA
 *                   continues with the next entry)
 *   LENGTH  > 0  -> decode PB entries of the segment at GET, respecting
 *                   FETCH_CONDITIONAL + the subdevice-mask filter, until
 *                   the segment is exhausted or END_PB_SEGMENT is hit.
 *
 * PB instruction decode (dev_ram.ref SEC_OP/TERT table + clc06f.h):
 *   SEC_OP 0, TERT 1/2/3   SET/STORE/USE_SUBDEVICE_MASK control entries
 *   SEC_OP 0, TERT 0       legacy (NVC0-era) INC method: COUNT_OLD 28:18,
 *                          ADDRESS_OLD 12:2, SUBCH 15:13 (NVC06F_DMA_
 *                          OPCODE_METHOD = 0)
 *   SEC_OP 2, TERT 0       legacy NON-INC method (OPCODE_NONINC_METHOD=2)
 *   SEC_OP 2, TERT != 0    no defined form => PBENTRY
 *   SEC_OP 1/3/4/5         modern INC / NON-INC / IMMD / ONE_INC headers
 *   SEC_OP 6               RESERVED (never valid) => PBENTRY
 *   SEC_OP 7               END_PB_SEGMENT: nothing further in this
 *                          segment is decoded
 *   word == 0              universal NOP (NV_FIFO_DMA_NOP)
 *
 * Method data may cross into following GP entries' segments (dev_ram.ref:
 * "PB method data entries may be located in a PB segment separate from
 * their corresponding method header"), so a segment that ends with data
 * still pending is NOT an error while another GP entry follows. Reaching
 * the last entry with data pending IS PG106_PBDMA_ERR_OVERRUN: the PBDMA
 * would stall with nowhere further to fetch. A method sequence crossing
 * into a FETCH_CONDITIONAL segment raises PBSEG (dev_pbdma.ref
 * INTR_0_PBSEG) and the run continues, as on hardware after the
 * interrupt is cleared.
 */
#include "pascal/pbdma.h"

#include <stdbool.h>
#include <string.h>

/* clc06f.h:233 NVC06F_DMA_METHOD_ADDRESS 11:0 (modern, 12 bits) /
 * clc06f.h:232 NVC06F_DMA_METHOD_ADDRESS_OLD 12:2 (legacy, 11 bits). */
#define PBDMA_ADDR_MAX_MODERN 0x0FFFu
#define PBDMA_ADDR_MAX_LEGACY 0x07FFu

/* dev_ram.ref NV_FIFO_DMA_SEC_OP values. */
#define SEC_OP_GRP0_TERT 0u
#define SEC_OP_INC      1u
#define SEC_OP_GRP2_TERT 2u
#define SEC_OP_NONINC   3u
#define SEC_OP_IMMD     4u
#define SEC_OP_ONEINC   5u
#define SEC_OP_RSVD6    6u
#define SEC_OP_ENDSEG   7u

/* clc06f.h NVC06F_GP_ENTRY1_OPCODE values (LENGTH==0 control entries). */
#define GP_OPCODE_NOP     0u
#define GP_OPCODE_ILLEGAL 1u
#define GP_OPCODE_GP_CRC  2u
#define GP_OPCODE_PB_CRC  3u

/* Minimal NV_PPBDMA_SUBDEVICE model (dev_pbdma.ref SUBDEVICE register):
 * the ID defaults to all-ones (NV_PPBDMA_SUBDEVICE_ID_ENABLE = 0xfff) so
 * an unfiltered stream is ACTIVE, which is the only mode anything modern
 * (nouveau / NVK / tinygrad / our encoders) ever uses. */
#define PBDMA_SUBDEVICE_ID 0xFFFu

/* Internal sentinel: the segment ended via END_PB_SEGMENT. */
#define SEG_END 100

typedef struct pbdma_state {
    pg106_pbdma_trace *t;
    pg106_pbdma_reader reader;

    /* NV_PPBDMA_SUBDEVICE model. */
    uint32_t subdevice_stored;  /* STORE_SUBDEVICE_MASK target          */
    bool     subdevice_active;  /* SUBDEVICE_STATUS                     */

    /* Pending method data (the NV_PPBDMA_PB_HEADER/PB_COUNT analog). */
    bool     pending;           /* header has words outstanding         */
    bool     inc;               /* INC: dword addr ++ per word          */
    bool     one_inc;           /* ONE_INC: ++ after the first word     */
    uint32_t mthd;              /* next data word's dword address       */
    uint32_t words_left;
    uint32_t addr_max;          /* wrap limit of this header's format   */
    size_t   hdr_index;         /* trace slot of the pending header     */
} pbdma_state;

int pg106_pbdma_trace_init(pg106_pbdma_trace *t,
                           pg106_pbdma_entry *entries, size_t cap,
                           uint32_t *data_pool, size_t pool_cap)
{
    if (t == NULL)
        return PG106_PBDMA_ERR_ARG;
    memset(t, 0, sizeof(*t));
    t->entries = entries;
    t->cap = cap;
    t->data_pool = data_pool;
    t->pool_cap = pool_cap;
    return PG106_PBDMA_OK;
}

/* Record the first error only (sticky), like the hardware INTR registers. */
static void trace_error(pbdma_state *st, int err, uint32_t gpfifo_index,
                        uint32_t word)
{
    if (st->t->error == PG106_PBDMA_OK) {
        st->t->error = err;
        st->t->error_gpfifo = gpfifo_index;
        st->t->error_word = word;
    }
}

static void trace_push(pbdma_state *st, const pg106_pbdma_entry *e,
                       size_t *index_out)
{
    if (st->t->len < st->t->cap) {
        *index_out = st->t->len;
        st->t->entries[st->t->len++] = *e;
    } else {
        *index_out = (size_t)-1; /* trace full: entry dropped */
    }
}

static void pool_push(pbdma_state *st, uint32_t word)
{
    if (st->t->pool_len < st->t->pool_cap)
        st->t->data_pool[st->t->pool_len++] = word;
}

static int pb_read(pbdma_state *st, uint64_t dword_addr, uint32_t *out)
{
    return st->reader.read(st->reader.ctx, dword_addr, out);
}

/*
 * Consume up to `avail` data words of the pending method from the current
 * segment. Returns the number of words consumed, or -1 on a reader fault;
 * the live data_len is written back into the header's trace entry.
 */
static int consume_data(pbdma_state *st, uint64_t seg_base, uint32_t pos,
                        uint32_t avail)
{
    uint32_t consumed = 0;
    while (avail-- > 0 && st->words_left > 0) {
        uint32_t word;
        if (pb_read(st, seg_base + pos, &word) != 0)
            return -1;
        pool_push(st, word);
        if (st->hdr_index < st->t->len)
            st->t->entries[st->hdr_index].data_len++;
        /* Address walk (feeds the PBENTRY wrap check only): INC advances
         * every word; ONE_INC advances from the second word on; NON-INC
         * never advances (dev_ram.ref per-format sections). */
        if (st->inc || (st->one_inc && consumed >= 1))
            st->mthd += 4;
        consumed++;
        st->words_left--;
        pos++;
    }
    return (int)consumed;
}

/*
 * Decode one PB instruction word into *e (gpfifo_index/pb_offset and the
 * data fields are filled by the caller). Returns 0, or
 * PG106_PBDMA_ERR_PBENTRY if the word does not decode (dev_pbdma.ref
 * INTR_0_PBENTRY: "the PB entry does not decode properly into a PB
 * instruction").
 */
static int decode_instruction(uint32_t word, pg106_pbdma_entry *e)
{
    unsigned sec_op  = (word >> 29) & 0x7u;     /* NVC06F_DMA_SEC_OP   */
    unsigned tert    = (word >> 16) & 0x3u;     /* TERT_OP / OPCODE3   */
    unsigned subch   = (word >> 13) & 0x7u;     /* SUBCHANNEL 15:13    */
    uint32_t count_m = (word >> 16) & 0x1FFFu;  /* METHOD_COUNT 28:16  */
    uint32_t mthd_m  = (word & PBDMA_ADDR_MAX_MODERN) << 2; /* ADDRESS 11:0 */

    memset(e, 0, sizeof(*e));
    e->secop = (uint8_t)sec_op;
    e->tert_op = (uint8_t)tert;
    e->subch = (uint8_t)subch;
    e->count_raw = count_m;
    e->mthd = mthd_m;
    e->count = (uint8_t)(count_m & 0xFFu);

    switch (sec_op) {
    case SEC_OP_GRP0_TERT:
        if (tert == 0) {
            /* Legacy incrementing method (NVC06F_DMA_OPCODE_METHOD=0):
             * COUNT_OLD 28:18, ADDRESS_OLD 12:2, SUBCH 15:13. */
            e->flags = PG106_PBDMA_FLAG_LEGACY_TERT;
            e->count_raw = (word >> 18) & 0x3FFu;
            e->mthd = ((word >> 2) & 0x7FFu) << 2;
            e->count = (uint8_t)(e->count_raw & 0xFFu);
            return PG106_PBDMA_OK;
        }
        /* 1/2/3: SET/STORE/USE_SUBDEVICE_MASK control entries
         * (dev_ram.ref SEC_OP/TERT instruction table). */
        e->flags = PG106_PBDMA_FLAG_CONTROL;
        e->mthd = tert;                    /* 1=SET 2=STORE 3=USE */
        e->count_raw = (word >> 4) & 0xFFFu; /* *_MASK_VALUE 15:4 */
        return PG106_PBDMA_OK;

    case SEC_OP_GRP2_TERT:
        if (tert != 0)
            return PG106_PBDMA_ERR_PBENTRY; /* no defined form */
        /* Legacy non-incrementing method (NVC06F_DMA_OPCODE_
         * NONINC_METHOD=2), same field layout as legacy INC. */
        e->flags = PG106_PBDMA_FLAG_LEGACY_TERT;
        e->count_raw = (word >> 18) & 0x3FFu;
        e->mthd = ((word >> 2) & 0x7FFu) << 2;
        e->count = (uint8_t)(e->count_raw & 0xFFu);
        return PG106_PBDMA_OK;

    case SEC_OP_INC:
    case SEC_OP_NONINC:
    case SEC_OP_IMMD:
    case SEC_OP_ONEINC:
        /* Modern headers; IMMD keeps its operand in count_raw/[28:16].
         * TERT_OP [17:16] is only meaningful in the legacy forms. */
        e->tert_op = 0;
        return PG106_PBDMA_OK;

    case SEC_OP_ENDSEG:
        return PG106_PBDMA_OK;

    case SEC_OP_RSVD6:
    default:
        return PG106_PBDMA_ERR_PBENTRY;
    }
}

/*
 * Begin a method header: check the PBENTRY address-wrap condition, set
 * the pending-data state, emit the header's trace entry. Control entries
 * and IMMD (which generate no follow-on data) never become pending.
 * Returns 0 or PG106_PBDMA_ERR_PBENTRY (fatal: hardware stalls).
 */
static int begin_method(pbdma_state *st, const pg106_pbdma_entry *e)
{
    if (st->hdr_index < st->t->len)
        st->t->entries[st->hdr_index].data_offset =
            (uint32_t)st->t->pool_len;
    st->inc = (e->secop == SEC_OP_INC ||
               (e->secop == SEC_OP_GRP0_TERT && e->tert_op == 0));
    st->one_inc = (e->secop == SEC_OP_ONEINC);
    st->addr_max = (e->flags & PG106_PBDMA_FLAG_LEGACY_TERT)
                       ? PBDMA_ADDR_MAX_LEGACY
                       : PBDMA_ADDR_MAX_MODERN;

    if (e->secop == SEC_OP_IMMD) {
        /* dev_ram.ref "Immediate-Data": exactly one method is generated,
         * its operand is COUNT [28:16]. Copy it into the pool so trace
         * reconstruction is uniform across header kinds. */
        pool_push(st, e->count_raw);
        if (st->hdr_index < st->t->len)
            st->t->entries[st->hdr_index].data_len = 1;
        return PG106_PBDMA_OK;
    }

    /* dev_pbdma.ref PBENTRY bullet 3: an INC/ONE_INC header whose COUNT
     * would wrap the method address does not decode. */
    if (st->inc && e->count_raw > 0 &&
        (e->mthd >> 2) > st->addr_max - (e->count_raw - 1))
        trace_error(st, PG106_PBDMA_ERR_PBENTRY, e->gpfifo_index,
                    e->mthd | (e->count_raw << 16));

    if (e->count_raw == 0) {
        /* dev_ram.ref INC/NON-INC/ONE-INC sections: COUNT==0 => "the
         * other fields are ignored, and the PB method effectively
         * becomes a no-op" - no data words are consumed. */
        if (st->hdr_index < st->t->len)
            st->t->entries[st->hdr_index].flags |=
                PG106_PBDMA_FLAG_COUNT0_NOP;
        return PG106_PBDMA_OK;
    }

    st->pending = true;
    st->mthd = e->mthd;
    st->words_left = e->count_raw;
    return PG106_PBDMA_OK;
}

/* Decode one PB instruction word (no method data pending). */
static int step_instruction(pbdma_state *st, uint32_t word,
                            uint32_t gpfifo_index, uint32_t pb_offset)
{
    pg106_pbdma_entry e;
    int rc = decode_instruction(word, &e);
    if (rc != PG106_PBDMA_OK) {
        trace_error(st, rc, gpfifo_index, word);
        return rc; /* PBENTRY: the hardware stalls, we stop the run */
    }

    e.gpfifo_index = gpfifo_index;
    e.pb_offset = pb_offset;

    if (e.flags & PG106_PBDMA_FLAG_CONTROL) {
        /* dev_pbdma.ref SUBDEVICE register semantics. */
        switch (e.mthd) {
        case 1: /* SET_SUBDEVICE_MASK */
            st->subdevice_active =
                (PBDMA_SUBDEVICE_ID & e.count_raw) != 0;
            break;
        case 2: /* STORE_SUBDEVICE_MASK */
            st->subdevice_stored = e.count_raw;
            break;
        case 3: /* USE_SUBDEVICE_MASK */
            st->subdevice_active =
                (PBDMA_SUBDEVICE_ID & st->subdevice_stored) != 0;
            break;
        default:
            break;
        }
        trace_push(st, &e, &st->hdr_index);
        return PG106_PBDMA_OK;
    }

    if (e.secop == SEC_OP_ENDSEG) {
        e.flags |= PG106_PBDMA_FLAG_ENDSEG;
        trace_push(st, &e, &st->hdr_index);
        return SEG_END;
    }

    trace_push(st, &e, &st->hdr_index);
    return begin_method(st, &e);
}

/*
 * Decode the PB entries of one segment at dword address `seg_base`,
 * `len_dwords` long. Returns 0 (fully consumed), SEG_END (END_PB_SEGMENT
 * hit), or a fatal error (FETCH / PBENTRY).
 */
static int run_segment(pbdma_state *st, uint64_t seg_base, uint32_t len_dwords,
                       uint32_t gpfifo_index, bool conditional)
{
    uint32_t pos = 0;
    int rc;

    /* Pending method data from the previous GP entry: dev_ram.ref says
     * method data may live in a following segment. If that following
     * segment is FETCH_CONDITIONAL, the sequence "begins in a non-
     * conditionally fetched PB segment and ends in a conditionally-
     * fetched PB segment" => INTR_0_PBSEG; hardware consumes the data
     * anyway once the interrupt is cleared ("the first valid PB entry of
     * a conditionally-fetched PB segment is interpreted as method data").
     */
    if (st->pending) {
        if (conditional)
            trace_error(st, PG106_PBDMA_ERR_PBSEG, gpfifo_index,
                        (uint32_t)seg_base);
        /* The header's data continued into this segment. */
        if (st->hdr_index < st->t->len)
            st->t->entries[st->hdr_index].flags |=
                PG106_PBDMA_FLAG_SEG_CROSS;
    }

    /* FETCH_CONDITIONAL + subdevice INACTIVE: nothing in this segment is
     * decoded or executed (dev_pbdma.ref GP_ENTRY0_FETCH + SUBDEVICE). */
    if (conditional && !st->subdevice_active && !st->pending) {
        pg106_pbdma_entry e;
        memset(&e, 0, sizeof(e));
        e.gpfifo_index = gpfifo_index;
        e.flags = PG106_PBDMA_FLAG_SEG_SKIPPED;
        trace_push(st, &e, &st->hdr_index);
        return PG106_PBDMA_OK;
    }

    while (pos < len_dwords) {
        if (st->pending) {
            int got = consume_data(st, seg_base, pos, len_dwords - pos);
            if (got < 0) {
                trace_error(st, PG106_PBDMA_ERR_FETCH, gpfifo_index,
                            (uint32_t)seg_base);
                return PG106_PBDMA_ERR_FETCH;
            }
            pos += (uint32_t)got;
            if (st->words_left == 0)
                st->pending = false;
            continue;
        }

        uint32_t word;
        if (pb_read(st, seg_base + pos, &word) != 0) {
            trace_error(st, PG106_PBDMA_ERR_FETCH, gpfifo_index,
                        (uint32_t)(seg_base + pos));
            return PG106_PBDMA_ERR_FETCH;
        }
        if (word == 0x00000000u) {
            /* Universal NOP: "bypasses any method header format check,
             * and is not considered a method header" (dev_ram.ref). */
            pos++;
            continue;
        }
        rc = step_instruction(st, word, gpfifo_index, pos);
        if (rc == SEG_END)
            return SEG_END;
        if (rc != PG106_PBDMA_OK)
            return rc; /* fatal: FETCH / PBENTRY */
        pos++;
    }
    return PG106_PBDMA_OK;
}

/* One GP entry: control entry or pushbuffer segment decode. */
static int run_gp_entry(pbdma_state *st, uint64_t entry, uint32_t gpfifo_index)
{
    uint64_t seg_addr;
    uint32_t len_dwords;
    unsigned level, sync, fetch, priv;

    pg106_gpfifo_unpack(entry, &seg_addr, &len_dwords, &level, &sync,
                        &fetch, &priv);
    /* entry1's OPCODE field (7:0) overlaps LENGTH 30:10 and is only
     * meaningful when LENGTH == 0 (dev_pbdma.ref GP_ENTRY). */
    unsigned opcode = (unsigned)(entry >> 32) & 0xFFu;

    if (len_dwords == 0) {
        pg106_pbdma_entry e;
        memset(&e, 0, sizeof(e));
        e.gpfifo_index = gpfifo_index;
        e.flags = PG106_PBDMA_FLAG_GP_CONTROL;
        e.mthd = opcode;
        trace_push(st, &e, &st->hdr_index);
        if (opcode == GP_OPCODE_ILLEGAL || opcode > GP_OPCODE_PB_CRC) {
            /* dev_pbdma.ref: "If the GP entry's opcode is OPCODE_ILLEGAL
             * or an invalid opcode, Host will initiate an interrupt
             * (NV_PPBDMA_INTR_0_GPENTRY)... the invalid GP entry is
             * discarded. Once the interrupt is cleared, the PBDMA unit
             * will simply continue with the next GP entry." */
            trace_error(st, PG106_PBDMA_ERR_GPENTRY, gpfifo_index,
                        (uint32_t)entry);
            return PG106_PBDMA_OK;
        }
        return PG106_PBDMA_OK; /* NOP / GP_CRC / PB_CRC: no PB data */
    }

    /* GET 33:2 => dword address = byte address >> 2. */
    return run_segment(st, seg_addr >> 2, len_dwords, gpfifo_index,
                       fetch == PG106_GP_FETCH_CONDITIONAL);
}

static int pbdma_run_n(const uint64_t *gpfifo_ring, uint32_t ring_entries,
                       uint32_t get, uint32_t n,
                       const pg106_pbdma_reader *reader,
                       pg106_pbdma_trace *t)
{
    pbdma_state st;
    uint32_t i;

    if (gpfifo_ring == NULL || reader == NULL || reader->read == NULL ||
        t == NULL)
        return PG106_PBDMA_ERR_ARG;
    if (ring_entries == 0 && n != 0)
        return PG106_PBDMA_ERR_ARG;

    memset(&st, 0, sizeof(st));
    st.t = t;
    st.reader = *reader;
    st.subdevice_active = true; /* unfiltered stream: ACTIVE */

    for (i = 0; i < n; i++) {
        uint32_t slot = (get + i) % ring_entries;
        int rc = run_gp_entry(&st, gpfifo_ring[slot], slot);
        if (rc == SEG_END)
            continue; /* ENDSEG only bounds its own segment */
        if (rc != PG106_PBDMA_OK)
            return rc; /* FETCH / PBENTRY: stall, stop the run */
        /* If data is still pending after the last entry, the PBDMA has
         * nowhere further to fetch: method data overrun. */
        if (i + 1 == n && st.pending) {
            trace_error(&st, PG106_PBDMA_ERR_OVERRUN, slot,
                        (uint32_t)st.mthd);
            return PG106_PBDMA_ERR_OVERRUN;
        }
    }
    return PG106_PBDMA_OK;
}

int pg106_pbdma_run(const uint64_t *gpfifo_ring, uint32_t gpfifo_count,
                    const pg106_pbdma_reader *reader,
                    pg106_pbdma_trace *out_trace)
{
    if (gpfifo_count == 0) {
        if (gpfifo_ring == NULL || reader == NULL || reader->read == NULL ||
            out_trace == NULL)
            return PG106_PBDMA_ERR_ARG;
        return PG106_PBDMA_OK; /* PUT == GET: nothing to run */
    }
    return pbdma_run_n(gpfifo_ring, gpfifo_count, 0, gpfifo_count, reader,
                       out_trace);
}

int pg106_pbdma_run_getput(const uint64_t *gpfifo_ring, uint32_t ring_entries,
                           uint32_t get, uint32_t put,
                           const pg106_pbdma_reader *reader,
                           pg106_pbdma_trace *out_trace)
{
    uint32_t n;

    if (ring_entries == 0)
        return PG106_PBDMA_ERR_ARG;
    if (get >= ring_entries || put >= ring_entries)
        return PG106_PBDMA_ERR_ARG;
    n = (put + ring_entries - get) % ring_entries; /* PUT == GET: empty */
    return pbdma_run_n(gpfifo_ring, ring_entries, get, n, reader,
                       out_trace);
}
