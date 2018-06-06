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
#include <sys/malloc.h>

#include <machine/bus.h>

#include <xen/gnttab.h>

struct bus_dma_tag_xen {
  bus_dma_tag_t parent;
  grant_ref_t *refs;
  int nrefs;
  domid_t domid;
};

struct xen_callback_arg {
	/*
	 * The callback function specified by the client driver. Using a void pointer
	 * and not bus_dmamap_callback_t so we can use this struct with both
	 * bus_dmamap_load() and bus_dmamap_load_mbuf().
	 */
	void *client_callback;
	/* The client driver's callback arg. */
	void *client_callback_arg;

	/* Xen's callback args */
	grant_ref_t *xen_arg;
  int nrefs;
  domid_t domid;
};

static int
xen_bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
		bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
		bus_dma_filter_t *filtfunc, void *filtfuncarg, bus_size_t maxsize,
		int nsegments, bus_size_t maxsegsz, int flags,
		bus_dma_lock_t *lockfunc, void *lockfuncarg, bus_dma_tag_t *dmat)
{
	domid_t domid;
  struct bus_dma_tag_xen *newtag;
	int i, error;

	if (maxsegsz < PAGE_SIZE) {
		return (EINVAL);
	}

  newtag = malloc(sizeof(struct bus_dma_tag_xen), M_DEVBUF, M_NOWAIT);
  if (newtag == NULL) {
    return (ENOMEM);
  }

	domid = flags >> 16;
	flags &= 0xffff;

  *dmat = NULL;

  /* Allocate a new dma tag. This tag is newtag's parent. */
	error = bus_dma_tag_create(parent, alignment, boundary, lowaddr, highaddr,
			filtfunc, filtfuncarg, maxsize, nsegments, maxsegsz, flags, lockfunc,
			lockfuncarg, &(newtag->parent));
	if (error) {
    free(newtag, M_DEVBUF);
    newtag = NULL;
		return (error);
	}

  newtag->nrefs = nsegments;
  newtag->domid = domid;

	/* Allocate the grant references for each segment. */
	newtag->refs = malloc(nsegments*sizeof(grant_ref_t), M_DEVBUF, M_NOWAIT);
	if (newtag->refs == NULL) {
		bus_dma_tag_destroy(newtag->parent);
    free(newtag, M_DEVBUF);
    newtag = NULL;
		return (ENOMEM);
	}

  *dmat = (bus_dma_tag_t)newtag;

	return (0);
}

static int
xen_bus_dma_tag_destroy(bus_dma_tag_t dmat)
{
  struct bus_dma_tag_xen *xentag;
  int error;

  xentag = (struct bus_dma_tag_xen *)dmat;

  /* Clean up the parent tag first. */
  error = bus_dma_tag_destroy(xentag->parent);
	if (error) {
		return (error);
	}

	/* Free the refs array. */
	free(xentag->refs, M_DEVBUF);

  /* Free the Xen tag. */
  free(xentag, M_DEVBUF);

	return (0);
}

static void
xen_bus_dmamap_load_callback(void *callback_arg, bus_dma_segment_t *segs,
		int nseg, int error)
{
	grant_ref_t *refs;
	struct xen_callback_arg *arg;
	bus_dmamap_callback_t *callback;
	int i;

	if (error) {
		(*callback)(arg->client_callback_arg, segs, nseg, error);
		return;
	}

	arg = callback_arg;

	refs = arg->xen_arg;
	callback = arg->client_callback;

	for (i = 0; i < nseg; i++) {
		shared[refs[i]].frame = segs[i].ds_addr;
		/* XXX Should I call wmb() for each iteration of the loop or is it ok if I
		 * call it just once after the loop. */
		wmb();
	}

	/* Time to call the client's callback. */
	(*callback)(arg->client_callback_arg, segs, nseg, error);
}

static void
xen_bus_dmamap_load_mbuf_callback(void *callback_arg, bus_dma_segment_t *segs,
		int nseg, bus_size_t mapsize, int error)
{
	grant_ref_t *refs;
	struct xen_callback_arg *arg;
	bus_dmamap_callback_t *callback;
	int i;

	if (error) {
		(*callback)(arg->client_callback_arg, segs, nseg, mapsize, error);
		return;
	}

	arg = callback_arg;

	refs = arg->xen_arg;
	callback = arg->client_callback;

	for (i = 0; i < nseg; i++) {
		shared[refs[i]].frame = segs[i].ds_addr;
		/* XXX Should I call wmb() for each iteration of the loop or is it ok if I
		 * call it just once after the loop. */
		wmb();
	}

	/* Time to call the client's callback. */
	(*callback)(arg->client_callback_arg, segs, nseg, mapsize, error);
}


static int
xen_bus_dmamap_load(bus_dma_tag_t dmat, bus_dmamap_t map, void	*buf,
		bus_size_t buflen, bus_dmamap_callback_t *callback,
		void *callback_arg, int flags, grant_ref_t *refs)
{
	struct xen_callback_arg arg;
	int error;

	arg.client_callback = callback;
	arg.client_callback_arg = callback_arg;
	arg.xen_arg = refs;

	error = bus_dmamap_load(dmat, map, buf, buflen, xen_bus_dmamap_load_callback,
			&arg, flags);
	if (error) {
		return (error);
	}

	return (0);
}

static int
xen_bus_dmamap_load_mbuf(bus_dma_tag_t	dmat, bus_dmamap_t map,
		struct mbuf *mbuf, bus_dmamap_callback2_t *callback,
		void *callback_arg, int flags, grant_ref_t *refs)
{
	struct xen_callback_arg arg;
	int error;

	arg.client_callback = callback;
	arg.client_callback_arg = callback_arg;
	arg.xen_arg = refs;

	error = bus_dmamap_load_mbuf(dmat, map, mbuf,
			xen_bus_dmamap_load_mbuf_callback, &arg, flags);
	if (error) {
		return (error);
	}

	return (0);
}

static void
xen_bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map, grant_ref_t *refs,
		unsigned int refcount)
{
	unsigned int i;

	for (i = 0; i < refcount; i++) {
		shared[refs[i]].frame = 0;
		wmb();
	}

	bus_dmamap_unload(dmat, map);
}
