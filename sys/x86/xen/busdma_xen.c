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

MALLOC_DEFINE(M_BUSDMA_XEN, "busdma_xen_buf", "Xen-specific bus_dma(9) buffer");
MALLOC_DEFINE(M_XEN_DMAMAP, "xen_dmamap", "Xen-specific DMA map");

struct bus_dma_tag_xen {
	struct bus_dma_tag_common common;
	bus_dma_tag_t parent;
	struct bus_dma_impl parent_impl;
	int nsegments;
	domid_t domid;
};

struct bus_dmamap_xen {
	bus_dmamap_t map;
	grant_ref_t gref_head;  /* The head of the grant references list. */
	grant_ref_t *refs;
	unsigned int nrefs;
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
	bus_dma_tag_t newparent;
	int error;

	if (maxsegsz < PAGE_SIZE) {
		return (EINVAL);
	}

	domid = flags >> 16;
	flags &= 0xffff;

	*dmat = NULL;

	/*
	 * We create two tags here. The first tag is the common tag. It will be used
	 * to hold the xen-specific bus_dma_impl. But, in the map create and load
	 * operations, we need to use the standard dma tag to load the dma maps and
	 * extract the physical addresses. So for those operations, we create another
	 * tag from the parent and use it in those operations.
	 */
	error = common_bus_dma_tag_create(NULL, alignment, boundary, lowaddr,
			highaddr, filtfunc, filtfuncarg, maxsize, nsegments, maxsegsz,
			flags, lockfunc, lockfuncarg, sizeof(struct bus_dma_tag_xen),
			(void **)&newtag);

	if (error) {
		return (error);
	}

	error = bus_dma_tag_create(parent, alignment, boundary, lowaddr,
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
	newtag->nsegments = nsegments;
	newtag->domid = domid;

	*dmat = (bus_dma_tag_t)newtag;

	return (0);
}

static int
xen_bus_dma_tag_destroy(bus_dma_tag_t dmat)
{
	struct bus_dma_tag_xen *xentag;
	int error;

	xentag = (struct bus_dma_tag_xen *)dmat;

	error = bus_dma_tag_destroy(xentag->parent);
	if (error) {
		return (error);
	}

	free(xentag, M_DEVBUF);

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

	xenmap = malloc(sizeof(struct bus_dmamap_xen), M_XEN_DMAMAP,
			M_NOWAIT | M_ZERO);
	if (xenmap == NULL) {
		return (ENOMEM);
	}

	error = bus_dmamap_create(xentag->parent, flags, &xenmap->map);
	if (error) {
		free(xenmap, M_XEN_DMAMAP);
		return (error);
	}

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

	KASSERT(xenmap->refs == NULL, ("busdma_xen: xenmap->refs not NULL"));

	free(xenmap, M_XEN_DMAMAP);
	return (0);
}

/* TODO: Figure out how to get these two to work. */
static int
xen_bus_dmamem_alloc(bus_dma_tag_t dmat, void** vaddr, int flags,
		bus_dmamap_t *mapp)
{
	struct bus_dma_tag_xen *xentag;

	xentag = (struct bus_dma_tag_xen *)dmat;

	return (bus_dmamem_alloc(xentag->parent, vaddr, flags, mapp));
}

static void
xen_bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map)
{
	struct bus_dma_tag_xen *xentag;

	xentag = (struct bus_dma_tag_xen *)dmat;

	return (bus_dmamem_free(xentag->parent, vaddr, map));
}

static int
xen_bus_dmamap_load_ma(bus_dma_tag_t dmat, bus_dmamap_t map,
		struct vm_page **ma, bus_size_t tlen, int ma_offs, int flags,
		bus_dma_segment_t *segs, int *segp)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	int error, segcount;
	unsigned int i;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	/*
	 * segp contains the starting segment on entrace, and the ending segment on
	 * exit. We can use it to calculate how many segments the map uses.
	 */
	segcount = *segp;

	error = _bus_dmamap_load_ma(xentag->parent, xenmap->map, ma, tlen, ma_offs,
			flags, segs, segp);
	if (error) {
		return (error);
	}

	/* XXX Should I even use this hack? Or should I simply do
	 * nrefs = xentag->nsegments? */
	segcount = *segp - segcount;
	xenmap->nrefs = (unsigned int)segcount;

	KASSERT(segcount <= xentag->nsegments, ("busdma_xen: segcount too large: "
			"segcount = %d, xentag->nsegments = %d", segcount, xentag->nsegments));

	xenmap->refs = malloc(xenmap->nrefs*sizeof(grant_ref_t),
			M_BUSDMA_XEN, M_NOWAIT);
	if (xenmap->refs == NULL) {
		/* Unload the map before returning. */
		bus_dmamap_unload(xentag->parent, xenmap->map);
		return (ENOMEM);
	}

	error = gnttab_alloc_grant_references(xenmap->nrefs, &xenmap->gref_head);
	if (error) {
		/* Unload the map before returning. */
		bus_dmamap_unload(xentag->parent, xenmap->map);
		free(xenmap->refs, M_BUSDMA_XEN);
		return (error);
	}

	/* Claim the grant references allocated and store them in the refs array. */
	for (i = 0; i < xenmap->nrefs; i++) {
		xenmap->refs[i] = gnttab_claim_grant_reference(&xenmap->gref_head);
	}

	return (0);
}

static int
xen_bus_dmamap_load_phys(bus_dma_tag_t dmat, bus_dmamap_t map,
		vm_paddr_t buf, bus_size_t buflen, int flags,
		bus_dma_segment_t *segs, int *segp)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	int error, segcount;
	unsigned int i;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	/*
	 * segp contains the starting segment on entrace, and the ending segment on
	 * exit. We can use it to calculate how many segments the map uses.
	 */
	segcount = *segp;

	error = _bus_dmamap_load_phys(xentag->parent, xenmap->map, buf, buflen,
			flags, segs, segp);
	if (error) {
		return (error);
	}

	segcount = *segp - segcount;
	xenmap->nrefs = (unsigned int)segcount;

	KASSERT(segcount <= xentag->nsegments, ("busdma_xen: segcount too large: "
			"segcount = %d, xentag->nsegments = %d", segcount, xentag->nsegments));

	xenmap->refs = malloc(xenmap->nrefs*sizeof(grant_ref_t),
			M_BUSDMA_XEN, M_NOWAIT);
	if (xenmap->refs == NULL) {
		/* Unload the map before returning. */
		bus_dmamap_unload(xentag->parent, xenmap->map);
		return (ENOMEM);
	}

	error = gnttab_alloc_grant_references(xenmap->nrefs, &xenmap->gref_head);
	if (error) {
		/* Unload the map before returning. */
		bus_dmamap_unload(xentag->parent, xenmap->map);
		free(xenmap->refs, M_BUSDMA_XEN);
		return (error);
	}

	/* Claim the grant references allocated and store them in the refs array. */
	for (i = 0; i < xenmap->nrefs; i++) {
		xenmap->refs[i] = gnttab_claim_grant_reference(&xenmap->gref_head);
	}

	return (0);
}

static int
xen_bus_dmamap_load_buffer(bus_dma_tag_t dmat, bus_dmamap_t map,
		void *buf, bus_size_t buflen, struct pmap *pmap, int flags,
		bus_dma_segment_t *segs, int *segp)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	int error, segcount;
	unsigned int i;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	/*
	 * segp contains the starting segment on entrace, and the ending segment on
	 * exit. We can use it to calculate how many segments the map uses.
	 */
	segcount = *segp;

	error = _bus_dmamap_load_buffer(xentag->parent, xenmap->map, buf, buflen,
			pmap, flags, segs, segp);
	if (error) {
		return (error);
	}

	segcount = *segp - segcount;
	xenmap->nrefs = (unsigned int)segcount;

	KASSERT(segcount <= xentag->nsegments, ("busdma_xen: segcount too large: "
			"segcount = %d, xentag->nsegments = %d", segcount, xentag->nsegments));

	xenmap->refs = malloc(xenmap->nrefs*sizeof(grant_ref_t),
			M_BUSDMA_XEN, M_NOWAIT);
	if (xenmap->refs == NULL) {
		/* Unload the map before returning. */
		bus_dmamap_unload(xentag->parent, xenmap->map);
		return (ENOMEM);
	}

	error = gnttab_alloc_grant_references(xenmap->nrefs, &xenmap->gref_head);
	if (error) {
		/* Unload the map before returning. */
		bus_dmamap_unload(xentag->parent, xenmap->map);
		free(xenmap->refs, M_BUSDMA_XEN);
		return (error);
	}

	/* Claim the grant references allocated and store them in the refs array. */
	for (i = 0; i < xenmap->nrefs; i++) {
		xenmap->refs[i] = gnttab_claim_grant_reference(&xenmap->gref_head);
	}

	return (0);
}

static void
xen_bus_dmamap_waitok(bus_dma_tag_t dmat, bus_dmamap_t map,
		struct memdesc *mem, bus_dmamap_callback_t *callback, void *callback_arg)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	_bus_dmamap_waitok(xentag->parent, xenmap->map, mem, callback, callback_arg);
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

	segs = _bus_dmamap_complete(xentag->parent, xenmap->map, segs, nsegs, error);

	/* If there was an error, do not map the grant references. */
	if (error) {
		return (segs);
	}

	for (i = 0; i < xenmap->nrefs; i++) {
		gnttab_grant_foreign_access_ref(refs[i], domid, segs[i].ds_addr, 0);
		segs[i].ds_addr = refs[i];
	}

	return (segs);
}

static void
xen_bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	struct bus_dma_tag_xen *xentag;
	struct bus_dmamap_xen *xenmap;
	grant_ref_t *refs;
	unsigned int i;

	xentag = (struct bus_dma_tag_xen *)dmat;
	xenmap = (struct bus_dmamap_xen *)map;

	refs = xenmap->refs;

	/* Reclaim the grant references. */
	for (i = 0; i < xenmap->nrefs; i++) {
		gnttab_end_foreign_access_ref(refs[i]);
		gnttab_release_grant_reference(&xenmap->gref_head, refs[i]);
	}

	free(xenmap->refs, M_BUSDMA_XEN);
	xenmap->refs = NULL;
	gnttab_free_grant_references(xenmap->gref_head);

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
