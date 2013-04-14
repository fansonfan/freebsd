/* $FreeBSD$ */
/*-
 * Copyright (c) 2011-2013 Spectra Logic Corporation
 * All rights reserved.
 *
 * This software was developed by Cherry G. Mathew <cherry@FreeBSD.org>
 * under sponsorship from Spectra Logic Corporation.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */


/*
 * This file implements the API that manages the page table
 * hierarchy for the amd64 Xen pmap.
 */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_cpu.h"
#include "opt_pmap.h"
#include "opt_smp.h"


#include <sys/libkern.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>

#include <xen/hypervisor.h>
#include <machine/xen/xenvar.h>

#include <amd64/xen/mmu_map.h>

static int
pml4t_index(uintptr_t va)
{
	/* amd64 sign extends 48th bit and upwards */
	const uint64_t SIGNMASK = (1UL << 48) - 1;
	va &= SIGNMASK; /* Remove sign extension */

	return (va >> PML4SHIFT); 
}

static int
pdpt_index(uintptr_t va)
{
	return ((va & PML4MASK) >> PDPSHIFT);
}

static int
pdt_index(uintptr_t va)
{
	return ((va & PDPMASK) >> PDRSHIFT);
}

#if 0
static int
pt_index(uintptr_t va)
{
	return ((va & PDRMASK) >> PAGE_SHIFT);
}
#endif

/* 
 * The table get functions below assume that a table cannot exist at
 * address 0
 */
static pml4_entry_t *
pmap_get_pml4t(struct pmap *pm)
{
	KASSERT(pm != NULL,
		("NULL pmap passed in!\n"));

	pml4_entry_t *pm_pml4 = pm->pm_pml4;
	
	KASSERT(pm_pml4 != NULL,
		("pmap has NULL pml4!\n"));

	return pm_pml4;
}

/* Returns physical address */
static vm_paddr_t
pmap_get_pdpt(uintptr_t va, pml4_entry_t *pml4t)
{
	pml4_entry_t pml4e;

	KASSERT(va <= VM_MAX_KERNEL_ADDRESS,
		("invalid address requested"));
	KASSERT(pml4t != 0, ("pml4t cannot be zero"));

	pml4e = pml4t[pml4t_index(va)];

	if (!(pml4e & PG_V)) {
		return 0;
	}

	return xpmap_mtop(pml4e & PG_FRAME);
}

/* Returns physical address */
static vm_paddr_t
pmap_get_pdt(uintptr_t va, pdp_entry_t *pdpt)
{
	pdp_entry_t pdpe;

	KASSERT(va <= VM_MAX_KERNEL_ADDRESS,
		("invalid address requested"));
	KASSERT(pdpt != 0, ("pdpt cannot be zero"));

	pdpe = pdpt[pdpt_index(va)];

	if (!(pdpe & PG_V)) {
		return 0;
	}

	return xpmap_mtop(pdpe & PG_FRAME);
}

/* Returns physical address */
static vm_paddr_t
pmap_get_pt(uintptr_t va, pd_entry_t *pdt)
{
	pd_entry_t pdte;

	KASSERT(va <= VM_MAX_KERNEL_ADDRESS,
		("invalid address requested"));

	KASSERT(pdt != 0, ("pdt cannot be zero"));

	pdte = pdt[pdt_index(va)];

	if (!(pdte & PG_V)) {
		return 0;
	}

	return xpmap_mtop(pdte & PG_FRAME);
}

/* 
 * This structure defines the 4 indices that a given virtual
 * address lookup would traverse.
 *
 * Note: this structure is opaque to API customers. Callers give us an
 * untyped array which is marshalled/unmarshalled inside of the
 * stateful api.
 */

static const uint32_t SANE = 0xcafebabe;

struct mmu_map_index {
	pml4_entry_t *pml4t; /* Page Map Level 4 Table */
	pdp_entry_t *pdpt;  /* Page Directory Pointer Table */
	pd_entry_t *pdt;   /* Page Directory Table */
	pt_entry_t *pt;    /* Page Table */

	struct mmu_map_mbackend ptmb; /* Backend info */

	uint32_t sanity; /* 32 bit (for alignment) magic XXX:
			  * Make optional on DEBUG */
};

size_t
mmu_map_t_size(void)
{
	return sizeof (struct mmu_map_index);
}

void
mmu_map_t_init(void *addr, struct mmu_map_mbackend *mb)
{
	KASSERT((addr != NULL) && (mb != NULL), ("NULL args given!"));
	struct mmu_map_index *pti = addr;
	KASSERT(pti->sanity != SANE, ("index initialised twice!"));
	KASSERT(mb->alloc != NULL &&
		mb->ptov != NULL &&
		mb->vtop != NULL, 
		("initialising with pre-registered alloc routine active"));

	pti->ptmb = *mb;

	/* Backend page allocation should provide default VA mapping */
	pti->sanity = SANE;
}

void mmu_map_t_fini(void *addr)
{
	KASSERT(addr != NULL, ("NULL args given!"));

	struct mmu_map_index *pti = addr;
	KASSERT(pti->sanity == SANE, ("%s: Uninitialised index cookie used", __func__));
	struct mmu_map_mbackend *mb = &pti->ptmb;

	pti->sanity = 0;

	if (mb->free != NULL) {
		/* XXX: go through PT hierarchy and free + unmap
		 * unused tables */ 
	}
}

pd_entry_t *
mmu_map_pml4t(void *addr)
{
	KASSERT(addr != NULL, ("NULL args given!"));
	struct mmu_map_index *pti = addr;

	KASSERT(pti->sanity == SANE, ("%s: Uninitialised index cookie used", __func__));

	return pti->pml4t;
}

pd_entry_t *
mmu_map_pdpt(void *addr)
{
	KASSERT(addr != NULL, ("NULL args given!"));
	struct mmu_map_index *pti = addr;

	KASSERT(pti->sanity == SANE, ("%s: Uninitialised index cookie used", __func__));

	return pti->pdpt;
}

pd_entry_t *
mmu_map_pdt(void *addr)
{
	KASSERT(addr != NULL, ("NULL args given!"));
	struct mmu_map_index *pti = addr;

	KASSERT(pti->sanity == SANE, ("%s: Uninitialised index cookie used", __func__));

	return pti->pdt;
}

pd_entry_t *
mmu_map_pt(void *addr)
{
	KASSERT(addr != NULL, ("NULL args given!"));
	struct mmu_map_index *pti = addr;

	KASSERT(pti->sanity == SANE, ("%s: Uninitialised index cookie used", __func__));

	return pti->pt;
}

bool
mmu_map_inspect_va(struct pmap *pm, void *addr, uintptr_t va)
{
	KASSERT(addr != NULL && pm != NULL, ("NULL arg(s) given"));

	struct mmu_map_index *pti = addr;
	KASSERT(pti->sanity == SANE, ("%s: Uninitialised index cookie used", __func__));

	vm_paddr_t pt;

	pti->pml4t = pmap_get_pml4t(pm);

	pt = pmap_get_pdpt(va, pti->pml4t);

	if (pt == 0) {
		return false;
	} else {
		pti->pdpt = (pdp_entry_t *) pti->ptmb.ptov(pt);
	}

	pt = pmap_get_pdt(va, pti->pdpt);

	if (pt == 0) {
		return false;
	} else {
		pti->pdt = (pd_entry_t *) pti->ptmb.ptov(pt);
	}

	pt = pmap_get_pt(va, pti->pdt);

	if (pt == 0) {
		return false;
	} else {
		pti->pt = (pt_entry_t *)pti->ptmb.ptov(pt);
	}

	return true;
}

bool
mmu_map_hold_va(struct pmap *pm, void *addr, uintptr_t va)
{
	KASSERT(addr != NULL && pm != NULL, ("NULL arg(s) given"));

	struct mmu_map_index *pti = addr;
	KASSERT(pti->sanity == SANE, ("%s: Uninitialised index cookie used", __func__));

	bool alloced = false; /* Did we have to alloc backing pages ? */
	vm_paddr_t pt;

	pti->pml4t = pmap_get_pml4t(pm);

	pt = pmap_get_pdpt(va, pti->pml4t);

	if (pt == 0) {
		pml4_entry_t *pml4tep;
		vm_paddr_t pml4tep_ma;
		pml4_entry_t pml4te;

		pti->pdpt = (pdp_entry_t *)pti->ptmb.alloc();
		alloced = true;

		pml4tep = &pti->pml4t[pml4t_index(va)];
		pml4tep_ma = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pml4tep));
		pml4te = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pti->pdpt)) | PG_RW | PG_V | PG_U; /* XXX: revisit flags */
		xen_queue_pt_update(pml4tep_ma, pml4te);
		xen_flush_queue();
	} else {
		pti->pdpt = (pdp_entry_t *) pti->ptmb.ptov(pt);
	}

	pt = pmap_get_pdt(va, pti->pdpt);

	if (pt == 0) {
		pdp_entry_t *pdptep;
		vm_paddr_t pdptep_ma;
		pdp_entry_t pdpte;

		pti->pdt = (pd_entry_t *)pti->ptmb.alloc();
		alloced = true;

		pdptep = &pti->pdpt[pdpt_index(va)];
		pdptep_ma = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pdptep));
		pdpte = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pti->pdt)) | PG_RW | PG_V | PG_U; /*	XXX: revisit flags */
		xen_queue_pt_update(pdptep_ma, pdpte);
		xen_flush_queue();
	} else {
		pti->pdt = (pd_entry_t *) pti->ptmb.ptov(pt);
	}

	pt = pmap_get_pt(va, pti->pdt);


	if (pt == 0) {
		pd_entry_t *pdtep;
		vm_paddr_t pdtep_ma;
		pd_entry_t pdte;

		pti->pt = (pt_entry_t *) pti->ptmb.alloc();
		alloced = true;

		pdtep = &pti->pdt[pdt_index(va)];
		pdtep_ma = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pdtep));
		pdte = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pti->pt)) | PG_RW | PG_V | PG_U; /*	XXX: revisit flags */
		xen_queue_pt_update(pdtep_ma, pdte);
		xen_flush_queue();
	} else {
		pti->pt = (pt_entry_t *) pti->ptmb.ptov(pt);
	}

	return alloced;
}

void
mmu_map_release_va(struct pmap *pm, void *addr, uintptr_t va)
{

	KASSERT(addr != NULL && pm != NULL, ("NULL arg(s) given"));

	struct mmu_map_index *pti = addr;
	KASSERT(pti->sanity == SANE, ("%s: Uninitialised index cookie used", __func__));

	/*
	 * We're expected to be called after an init-ed pti has either
	 * been inspected, or held.
	 */

	pti->pml4t = pmap_get_pml4t(pm);

	if (pti->pml4t == 0) {
		return;
	}

	if (pti->pt != NULL) {
		KASSERT(pti->pdt != 0, ("Invalid pdt\n"));
	}

	if (pti->pdt != NULL) { /* Zap pdte */

		pd_entry_t *pdtep;
		vm_paddr_t pdtep_ma;

		pdtep = &pti->pdt[pdt_index(va)];

		if (pti->pt == NULL) {
			KASSERT(*pdtep == 0, ("%s(%d): mmu state machine out of sync!\n", __func__, __LINE__));
		} else {

			/* 
			 * Corner case where the va ptes are mapped to
			 * itself (within a page boundary) at L1
			 */
			if (atop((uintptr_t) pti->pt) == atop(va)) {
				/* Note: We assume that pti->pxxt are obtained via PTOV() macros */

				/* 
				 * There's nothing to do at this point
				 * since the pte may have been zapped
				 * and the mapping may be invalid
				 * now - in which case we can't
				 * attempt to (even read) access
				 * it. Simply return.
				 */
				return;
			}

			/* We can free the PT only after the PDT entry is zapped */
			if (memcchr(pti->pt, 0, PAGE_SIZE) == NULL) {
				/* Zap the backing PDT entry */
				pdtep_ma = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pdtep));
				xen_queue_pt_update(pdtep_ma, 0);
				xen_flush_queue();

				/* The PT is empty. Free it and zero the
				 * pointer */
				if (pti->ptmb.free) pti->ptmb.free((uintptr_t)pti->pt);
				pti->pt = NULL;

			}

		}

		KASSERT(pti->pdpt != 0, ("Invalid pdpt\n"));
	}

	if (pti->pdpt != NULL) { /* Zap pdpte */

		pdp_entry_t *pdptep;
		vm_paddr_t pdptep_ma;

		pdptep = &pti->pdpt[pdpt_index(va)];

		if (pti->pdt == NULL) {
			KASSERT(*pdptep == 0, ("%s(%d): mmu state machine out of sync!\n", __func__, __LINE__));
		}

		/* 
		 * Corner case where the va pdtes are mapped to
		 * itself (within a page boundary) at L2
		 */
		if (atop((uintptr_t) pti->pdt) == atop(va)) {
			/* Note: We assume that pti->pxxt are obtained via PTOV() macros */

			/* 
			 * There's nothing to do at this point
			 * since the pdte may have been zapped
			 * and the mapping may be invalid
			 * now - in which case we can't
			 * attempt to (even read) access
			 * it. Simply return.
			 */
			return;
		}

		/* We can free the PDT only after the PDPT entry is zapped */
		if (memcchr(pti->pt, 0, PAGE_SIZE) == NULL) {
			pdptep_ma = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pdptep));
			xen_queue_pt_update(pdptep_ma, 0);
			xen_flush_queue();

			/* The PDT is empty. Free it and zero the
			 * pointer 
			 */
			if (pti->ptmb.free) pti->ptmb.free((uintptr_t)pti->pdt);
			pti->pdt = NULL;
		}
		KASSERT(pti->pml4t != 0, ("Invalid pml4t\n"));
	}

	if (pti->pml4t != NULL) { /* Zap pml4te */
		pml4_entry_t *pml4tep;
		vm_paddr_t pml4tep_ma;

		pml4tep = &pti->pml4t[pml4t_index(va)];

		if (pti->pdpt == NULL) {
			KASSERT(*pml4tep == 0, ("%s(%d): mmu state machine out of sync!\n", __func__, __LINE__));
		}

		/*
		 * Corner case where the va pdptes are mapped to
		 * itself (within a page boundary) at L3
		 */
		if (atop((uintptr_t) pti->pdpt) == atop(va)) {
			/* Note: We assume that pti->pxxt are obtained via PTOV() macros */

			/* 
			 * There's nothing to do at this point
			 * since the pdpte may have been zapped
			 * and the mapping may be invalid
			 * now - in which case we can't
			 * attempt to (even read) access
			 * it. Simply return.
			 */
			return;
		}

		if (memcchr(pti->pt, 0, PAGE_SIZE) == NULL) {
			pml4tep_ma = xpmap_ptom(pti->ptmb.vtop((uintptr_t)pml4tep)
);
			xen_queue_pt_update(pml4tep_ma, 0);
			xen_flush_queue();

			/* The PDPT is empty. Free it and zero the
			 * pointer 
			 */
			if (pti->ptmb.free) pti->ptmb.free((uintptr_t)pti->pdpt);
			pti->pdpt = NULL;
		}
	}			

	/* 
	 * We leave the pml4t to be managed by pmap, since there are
	 * higher level aliasing issues across pmaps and vcpus that
	 * can't be addressed here.
	 */
}
					
