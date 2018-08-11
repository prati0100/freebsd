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
 * $FreeBSD$
 */

#ifndef __XEN_BUSDMA_H
#define __XEN_BUSDMA_H

/*
 * Amount of shift needed to encode/decode grant table flags in dma load flags.
 * It is used by the busdma implementation functions xen_bus_dmamap_load_ma(),
 * xen_bus_dmamap_load_phys(), xen_bus_dmamap_load_buffer() to decode the
 * flags that should be passed when doing grant table operations. The drivers
 * should not directly use this value, and instead use BUS_DMA_XEN_RO.
 */
#define BUS_DMA_XEN_GNTTAB_FLAGS_SHIFT 16

/*
 * This flag can be used by drivers to indicate that read-only access should be
 * granted to the pages. They should do something like:
 *
 * flags = your_busdma_flags | BUS_DMA_XEN_RO;
 */
#define BUS_DMA_XEN_RO (1u << BUS_DMA_XEN_GNTTAB_FLAGS_SHIFT)

/*
 * Amount of shift needed to encode/decode domin ID in dma tag create flags.
 * Used by xen_bus_dma_tag_create() to decode the domid from the flags passed.
 * The client drivers should use this to encode the domid in the flags parameter
 * passed to bus_dma_tag_create() by doing something like:
 *
 * flags = your_busdma_flags | (otherend_id << BUS_DMA_XEN_DOMID_SHIFT);
 */
#define BUS_DMA_XEN_DOMID_SHIFT 16

/*
 * The drivers can pass it to map_create()'s flags to pre-allocate grant refs so
 * they don't have to deal with failed grant reference allocation when loading.
 * The number of grant references allocated is equal to the maximum number of
 * segments passed upon tag creation.
 *
 * BUS_DMA_BUS2 is reserved for the busdma implementations to use as they wish.
 *
 * Note: Grant references are a scarce resource. Try to not pre-allocate too
 * many grant references, or it might end up hindering other drivers.
 */
#define BUS_DMA_XEN_PREALLOC_REFS BUS_DMA_BUS2

bus_dma_tag_t xen_get_dma_tag(bus_dma_tag_t parent);

/*
 * Get the grant references corresponding to MAP.
 *
 * The number of grant references is equal to the number of segments, which is
 * passed to the callback specified when loading the map.
 *
 * Note: Do not modify the grant reference array returned in any way. It is
 * allocated and freed by the dmamap, and it should be considered read-only by
 * the client drivers.
 */
grant_ref_t *xen_dmamap_get_grefs(bus_dmamap_t map);

#endif /* __XEN_BUSDMA_H */
