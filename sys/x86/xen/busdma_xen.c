/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2018 Pratyush Yadav <pratyush@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <x86/include/busdma_impl.h>

#include <xen/gnttab.h>
#include <xen/busdma_xen.h>

MALLOC_DEFINE(M_BUSDMA_XEN, "busdma_xen_buf", "Xen-specific bus_dma(9) buffer");

/* BUS_DMA_BUS1 is reserved for bus functions to use as they wish. */
#define BUSDMA_XEN_TAG_INIT BUS_DMA_BUS1

struct bus_dma_tag_xen {
	struct bus_dma_tag_common common;
	bus_dma_tag_t parent;
	struct bus_dma_impl parent_impl;
	unsigned int max_segments;
	domid_t domid;
};

struct bus_dmamap_xen {
	struct bus_dma_tag_xen *tag;
	bus_dmamap_t map;
	grant_ref_t *refs;
	unsigned int nrefs;
	bus_dmamap_callback_t *callback;
	void *callback_arg;
	struct gnttab_free_callback gnttab_callback;
	bus_dma_segment_t *temp_segs;

	/* Flags. */
	bool sleepable;
	int gnttab_flags;
};

enum xen_load_type {
	LOAD_MA,
	LOAD_PHYS,
	LOAD_BUFFER,
	NOLOAD
};

struct load_op {
	enum xen_load_type type;
	bus_size_t size;
	int flags;
	bus_dma_segment_t *segs;
	int *segp;
	union {
		struct {
			struct vm_page **ma;
			unsigned int ma_offs;
		} ma;
		struct {
			vm_paddr_t buf;
		} phys;
		struct {
			void *buf;
			struct pmap *pmap;
		} buffer;
	};
};

struct bus_dma_impl bus_dma_xen_impl;

static int
xen_bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
		bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
		bus_dma_filter_t *filtfunc, void *filtfuncarg, bus_size_t maxsize,
		int nsegments, bus_size_t maxsegsz, int flags,
		bus_dma_lock_t *lockfunc, void *lockfuncarg, bus_dma_tag_t *dmat)
{
	domid_t domid;
	struct bus_dma_tag_xen *newtag;
	bus_dma_tag_t newparent, oldparent;
	int error;

	if (maxsegsz < PAGE_SIZE) {
		return (EINVAL);
	}

	domid = flags >> BUS_DMA_XEN_DOMID_SHIFT;
	flags &= 0xffff;

	if (flags & BUSDMA_XEN_TAG_INIT) {
		oldparent = parent;
	}
	else {
		oldparent = ((struct bus_dma_tag_xen *)parent)->parent;
	}

	*dmat = NULL;

	/*
	 * We create two tags here. The first tag is the common tag. It will be used
	 * to hold the xen-specific bus_dma_impl. But, in the map create and load
	 * operations, we need to use the standard dma tag to load the dma maps and
	 * extract the physical addresses. So for those operations, we create
	 * another tag from the parent and use it in those operations.
	 */
	error = common_bus_dma_tag_create(parent, alignment, boundary, lowaddr,
			highaddr, filtfunc, filtfuncarg, maxsize, nsegments, maxsegsz,
			flags, lockfunc, lockfuncarg, sizeof(struct bus_dma_tag_xen),
			(void **)&newtag);

	if (error) {
		return (error);
	}

	error = bus_dma_tag_create(oldparent, alignment, boundary, lowaddr,
			highaddr, filtfunc, filtfuncarg, maxsize, nsegments, maxsegsz,
			flags, lockfunc, lockfuncarg, &newparent);
	if (error) {
		bus_dma_tag_destroy((bus_dma_tag_t)newtag);
		return (error);
	}

	newtag->common.impl = &bus_dma_xen_impl;
	/* Save a copy of parent's impl. */
	newtag->parent_impl = *(((struct bus_dma_tag_common *)parent)->impl);
	newtag->parent = newparent;
	newtag->max_segments = nsegments;
	newtag->domid = domid;

	*dmat = (bus_dma_tag_t)newtag;

	return (0);
}

static int
xen_bus_dma_tag_destroy(bus_dma_tag_t dmat)
{
	struct bus_dma_tag_xen *xentag, *xenparent;
	int error;

	xentag = (struct bus_dma_tag_xen *)dmat;

	error = bus_dma_tag_destroy(xentag->parent);
	if (error) {
		return (error);
	}

	while (xentag != NULL) {
		xenparent = (struct bus_dma_tag_xen *)xentag->common.parent;
		if (atomic_fetchadd_int(&xentag->common.ref_count, -1) == 1) {
			free(xentag, M_DEVBUF);
			/*
			 * Last reference count, so
			 * release our reference
			 * count on our parent.
			 */
			xentag = xenparent;
		} else
			xentag = NULL;
	}

	return (0);
}

static int
xen_bus_dma_tag_set_domain(bus_dma_tag_t dmat)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dma_tag_common *parent;

	xentag = (struct bus_dma_tag_xen *)dmat;
	parent = (struct bus_dma_tag_common *)xentag->parent;

	return (parent->impl->tag_set_domain(xentag->parent));
}

static int
xen_bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	int error;

	xentag = (struct bus_dma_tag_xen *)dmat;

	/* mapp should NULL in case of an error. */
	*mapp = NULL;

	xenmap = malloc(sizeof(struct bus_dmamap_xen), M_BUSDMA_XEN,
			M_NOWAIT | M_ZERO);
	if (xenmap == NULL) {
		return (ENOMEM);
	}

	error = bus_dmamap_create(xentag->parent, flags, &xenmap->map);
	if (error) {
		free(xenmap, M_BUSDMA_XEN);
		return (error);
	}

	xenmap->tag = xentag;

	*mapp = (bus_dmamap_t)xenmap;
	return (0);
}

static int
xen_bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	int error;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	error = bus_dmamap_destroy(xentag->parent, xenmap->map);
	if (error) {
		return (error);
	}

	KASSERT(xenmap->refs == NULL,
			("busdma_xen: xenmap->refs not NULL. Check if unload was called"));

	free(xenmap, M_BUSDMA_XEN);
	return (0);
}

static int
xen_bus_dmamem_alloc(bus_dma_tag_t dmat, void** vaddr, int flags,
		bus_dmamap_t *mapp)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	int error;

	xentag = (struct bus_dma_tag_xen *)dmat;

	/* mapp should NULL in case of an error. */
	*mapp = NULL;

	xenmap = malloc(sizeof(struct bus_dmamap_xen), M_BUSDMA_XEN,
			M_NOWAIT | M_ZERO);
	if (xenmap == NULL) {
		return (ENOMEM);
	}

	error = bus_dmamem_alloc(xentag->parent, vaddr, flags, &xenmap->map);
	if (error) {
		free(xenmap, M_BUSDMA_XEN);
		return (error);
	}

	*mapp = (bus_dmamap_t)xenmap;
	return (0);
}

static void
xen_bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	bus_dmamem_free(xentag->parent, vaddr, xenmap->map);

	KASSERT(xenmap->refs == NULL,
			("busdma_xen: xenmap->refs not NULL. Check if unload was called"));

	free(xenmap, M_BUSDMA_XEN);
}

static void
xen_gnttab_free_callback(void *arg)
{
	struct bus_dmamap_xen *xenmap;
	struct bus_dma_tag_xen *xentag;
	grant_ref_t gref_head, *refs;
	bus_dma_segment_t *segs;
	bus_dmamap_callback_t *callback;
	domid_t domid;
	int error;
	unsigned int i;

	xenmap = arg;
	xentag = xenmap->tag;
	refs = xenmap->refs;
	domid = xentag->domid;
	callback = xenmap->callback;

	error = gnttab_alloc_grant_references(xenmap->nrefs, &gref_head);
	KASSERT((error == 0), ("busdma_xen: allocation of grant refs in the grant "
			"table free callback failed."));

	segs = xenmap->temp_segs;
	KASSERT((segs != NULL),
			("busdma_xen: %s: xenmap->temp_segs = NULL" , __func__));

	for (i = 0; i < xenmap->nrefs; i++) {
		xenmap->refs[i] = gnttab_claim_grant_reference(&gref_head);
		gnttab_grant_foreign_access_ref(refs[i], domid, segs[i].ds_addr,
				xenmap->gnttab_flags);
		segs[i].ds_addr = refs[i];
	}

	(xentag->common.lockfunc)(xentag->common.lockfuncarg, BUS_DMA_LOCK);
	(*callback)(xenmap->callback_arg, segs, xenmap->nrefs, 0);
	(xentag->common.lockfunc)(xentag->common.lockfuncarg,
	    BUS_DMA_UNLOCK);

	/* We don't need the temp_segs array anymore. */
	free(xenmap->temp_segs, M_BUSDMA_XEN);
	xenmap->temp_segs = NULL;

	return;
}

/* An internal function that is a helper for the three load variants. */
static int
xen_load_helper(struct bus_dma_tag_xen *xentag, struct bus_dmamap_xen *xenmap,
		struct load_op op)
{
	int error, segcount;
	unsigned int i;
	/* The head of the grant ref list used for batch allocating the refs. */
	grant_ref_t gref_head;

	xenmap->gnttab_flags = op.flags >> BUS_DMA_XEN_GNTTAB_FLAGS_SHIFT;
	op.flags &= 0xFFFF;

	KASSERT((xenmap->refs == NULL),
		("%s: Load called on an already loaded map? It is not supported yet.",
		__func__));

	/*
	 * segp contains the starting segment on entrace, and the ending segment on
	 * exit. We can use it to calculate how many segments the map uses.
	 */
	segcount = *op.segp;

	switch (op.type) {
		case LOAD_MA:
		{
			struct vm_page **ma = op.ma.ma;
			int ma_offs = op.ma.ma_offs;

			error = _bus_dmamap_load_ma(xentag->parent, xenmap->map, ma,
					op.size, ma_offs, op.flags, op.segs, op.segp);
			break;
		}
		case LOAD_PHYS:
		{
			vm_paddr_t buf = op.phys.buf;

			error = _bus_dmamap_load_phys(xentag->parent, xenmap->map, buf,
					op.size, op.flags, op.segs, op.segp);
			break;
		}
		case LOAD_BUFFER:
		{
			void *buf = op.buffer.buf;
			struct pmap *pmap = op.buffer.pmap;

			error = _bus_dmamap_load_buffer(xentag->parent, xenmap->map, buf,
					op.size, pmap, op.flags, op.segs, op.segp);
			break;
		}
		case NOLOAD:
			error = 0;
			break;
	}

	if (error == EINPROGRESS) {
		return (error);
	}
	else if (error != 0) {
		goto err;
	}

	if (op.type != NOLOAD) {
		segcount = *op.segp - segcount;
		xenmap->nrefs = segcount;

		KASSERT(segcount <= xentag->max_segments, ("busdma_xen: segcount too"
				" large: segcount = %d, xentag->max_segments = %d", segcount,
				xentag->max_segments));
	}

	xenmap->refs = malloc(xenmap->nrefs*sizeof(grant_ref_t),
			M_BUSDMA_XEN, M_NOWAIT);
	if (xenmap->refs == NULL) {
		error = ENOMEM;
		goto err;
	}

	error = gnttab_alloc_grant_references(xenmap->nrefs, &gref_head);
	if (error) {
		if (!xenmap->sleepable) {
			free(xenmap->refs, M_BUSDMA_XEN);
			xenmap->refs = NULL;
			goto err;
		}

		if (xenmap->temp_segs == NULL) {
			xenmap->temp_segs = malloc(xenmap->nrefs*sizeof(bus_dma_segment_t),
					M_BUSDMA_XEN, M_NOWAIT);
			if (xenmap->temp_segs == NULL) {
				free(xenmap->refs, M_BUSDMA_XEN);
				xenmap->refs = NULL;
				error = ENOMEM;
				goto err;
			}

			/* Complete the parent's load cycle by calling map_complete. */
			bus_dma_segment_t *segs = _bus_dmamap_complete(xentag->parent,
					xenmap->map, NULL, xenmap->nrefs, 0);

			/* Save a copy of the segs array, we need it later. */
			for (i = 0; i < xenmap->nrefs; i++) {
				xenmap->temp_segs[i] = segs[i];
			}
		}

		/*
		 * Request a free callback so we will be notified when the grant refs
		 * are available.
		 */
		gnttab_request_free_callback(&xenmap->gnttab_callback,
				xen_gnttab_free_callback, xenmap,
				xenmap->nrefs);

		return (EINPROGRESS);
	}

	/* Claim the grant references allocated and store them in the refs array. */
	for (i = 0; i < xenmap->nrefs; i++) {
		xenmap->refs[i] = gnttab_claim_grant_reference(&gref_head);
	}

	return (0);

	err:
		/* Unload the map before returning. */
		bus_dmamap_unload(xentag->parent, xenmap->map);
		return (error);
}

static int
xen_bus_dmamap_load_ma(bus_dma_tag_t dmat, bus_dmamap_t map,
		struct vm_page **ma, bus_size_t tlen, int ma_offs, int flags,
		bus_dma_segment_t *segs, int *segp)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	struct load_op op;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	op.type = LOAD_MA;
	op.size = tlen;
	op.flags = flags;
	op.segs = segs;
	op.segp = segp;
	op.ma.ma = ma;
	op.ma.ma_offs = ma_offs;

	return (xen_load_helper(xentag, xenmap, op));
}

static int
xen_bus_dmamap_load_phys(bus_dma_tag_t dmat, bus_dmamap_t map,
		vm_paddr_t buf, bus_size_t buflen, int flags,
		bus_dma_segment_t *segs, int *segp)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	struct load_op op;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	op.type = LOAD_PHYS;
	op.size = buflen;
	op.flags = flags;
	op.segs = segs;
	op.segp = segp;
	op.phys.buf = buf;

	return (xen_load_helper(xentag, xenmap, op));
}

static int
xen_bus_dmamap_load_buffer(bus_dma_tag_t dmat, bus_dmamap_t map,
		void *buf, bus_size_t buflen, struct pmap *pmap, int flags,
		bus_dma_segment_t *segs, int *segp)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	struct load_op op;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	op.type = LOAD_BUFFER;
	op.size = buflen;
	op.flags = flags;
	op.segs = segs;
	op.segp = segp;
	op.buffer.buf = buf;
	op.buffer.pmap = pmap;

	return (xen_load_helper(xentag, xenmap, op));
}

/*
 * If the load is called with the flag BUS_DMA_WAITOK, and the allocation is
 * deferred, the grant references need to be allocated before calling the
 * client's callback.
 */
static void
xen_dmamap_callback(void *callback_arg, bus_dma_segment_t *segs, int nseg,
		int error)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	bus_dmamap_callback_t *callback;
	grant_ref_t *refs;
	domid_t domid;
	unsigned int i;
	struct load_op op;

	xenmap = callback_arg;
	xentag = xenmap->tag;
	callback = xenmap->callback;
	domid = xentag->domid;

	xenmap->nrefs = nseg;

	if (error) {
		(*callback)(xenmap->callback_arg, segs, nseg, error);
		return;
	}

	/*
	 * Save a copy of the segs array. This may get over-written when another
	 * load on the same tag is called.
	 */
	xenmap->temp_segs = malloc(nseg*sizeof(bus_dma_segment_t), M_BUSDMA_XEN,
			M_NOWAIT);
	if (xenmap->temp_segs == NULL) {
		(*callback)(xenmap->callback_arg, segs, nseg, (ENOMEM));
		return;
	}

	for (i = 0; i < xenmap->nrefs; i++) {
		xenmap->temp_segs[i] = segs[i];
	}

	op.type = NOLOAD;

	error = xen_load_helper(xenmap->tag, xenmap, op);
	if (error == EINPROGRESS) {
		return;
	}
	else if (error != 0){
		free(xenmap->temp_segs, M_BUSDMA_XEN);
		xenmap->temp_segs = NULL;
		(*callback)(xenmap->callback_arg, segs, nseg, error);
		return;
	}

	/* We don't need temp_segs any more. */
	free(xenmap->temp_segs, M_BUSDMA_XEN);
	xenmap->temp_segs = NULL;

	refs = xenmap->refs;

	for (i = 0; i < xenmap->nrefs; i++) {
		gnttab_grant_foreign_access_ref(refs[i], domid, segs[i].ds_addr,
				xenmap->gnttab_flags);
		segs[i].ds_addr = refs[i];
	}

	(*callback)(xenmap->callback_arg, segs, nseg, 0);
}

static void
xen_bus_dmamap_waitok(bus_dma_tag_t dmat, bus_dmamap_t map,
		struct memdesc *mem, bus_dmamap_callback_t *callback,
		void *callback_arg)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	xenmap->callback = callback;
	xenmap->callback_arg = callback_arg;
	xenmap->sleepable = true;

	/*
	 * Some extra work has to be done before calling the client callback from
	 * a deferred context. When the load gets deferred, the grant references
	 * are not allocated. xen_dmamap_callback allocates the grant refs before
	 * calling the client's callback.
	 */
	_bus_dmamap_waitok(xentag->parent, xenmap->map, mem, xen_dmamap_callback,
			xenmap);
}

static bus_dma_segment_t *
xen_bus_dmamap_complete(bus_dma_tag_t dmat, bus_dmamap_t map,
		bus_dma_segment_t *segs, int nsegs, int error)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	grant_ref_t *refs;
	domid_t domid;
	unsigned int i;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;
	refs = xenmap->refs;
	domid = xentag->domid;

	segs = _bus_dmamap_complete(xentag->parent, xenmap->map, segs, nsegs,
			error);

	/* If there was an error, do not map the grant references. */
	if (error) {
		return (segs);
	}

	/* TODO: Take segp into account in this loop. */
	for (i = 0; i < xenmap->nrefs; i++) {
		gnttab_grant_foreign_access_ref(refs[i], domid, segs[i].ds_addr,
				xenmap->gnttab_flags);
		segs[i].ds_addr = refs[i];
	}

	return (segs);
}

/* XXX If the map is unloaded when the load has not completed, and allocation of
 * grant references has been defered, we might cause a segmentation fault by
 * accessing xenmap->refs. We will also leak grant refs. */
static void
xen_bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	grant_ref_t *refs;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	refs = xenmap->refs;

	gnttab_end_foreign_access_references(xenmap->nrefs, xenmap->refs);

	free(xenmap->refs, M_BUSDMA_XEN);
	xenmap->refs = NULL;

	/* Reset the flags. */
	xenmap->sleepable = false;

	KASSERT((xenmap->temp_segs == NULL),
			("busdma_xen: %s: xenmap->temp_segs not NULL.", __func__));

	bus_dmamap_unload(xentag->parent, xenmap->map);
}

static void
xen_bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map, bus_dmasync_op_t op)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	bus_dmamap_sync(xentag->parent, xenmap->map, op);
}

bus_dma_tag_t
xen_get_dma_tag(bus_dma_tag_t parent)
{
	bus_dma_tag_t newtag;
	bus_addr_t maxaddr;
	int error;

	maxaddr = BUS_SPACE_MAXADDR;

	error = xen_bus_dma_tag_create(parent,
			PAGE_SIZE, PAGE_SIZE,		/* alignment, boundary */
			maxaddr,				/* lowaddr */
			maxaddr,				/* highaddr */
			NULL, NULL,				/* filtfunc, filtfuncarg */
			maxaddr,				/* maxsize */
			BUS_SPACE_UNRESTRICTED,	/* nsegments */
			maxaddr,				/* maxsegsz */
			BUSDMA_XEN_TAG_INIT,	/* flags */
			NULL, NULL,				/* lockfunc, lockfuncarg */
			&newtag);

	return (newtag);
}

struct bus_dma_impl bus_dma_xen_impl = {
	.tag_create = xen_bus_dma_tag_create,
	.tag_destroy = xen_bus_dma_tag_destroy,
	.tag_set_domain = xen_bus_dma_tag_set_domain,
	.map_create = xen_bus_dmamap_create,
	.map_destroy = xen_bus_dmamap_destroy,
	.mem_alloc = xen_bus_dmamem_alloc,
	.mem_free = xen_bus_dmamem_free,
	.load_phys = xen_bus_dmamap_load_phys,
	.load_buffer = xen_bus_dmamap_load_buffer,
	.load_ma = xen_bus_dmamap_load_ma,
	.map_waitok = xen_bus_dmamap_waitok,
	.map_complete = xen_bus_dmamap_complete,
	.map_unload = xen_bus_dmamap_unload,
	.map_sync = xen_bus_dmamap_sync,
};
