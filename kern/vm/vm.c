/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <cpu.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <clock.h>
#include <synch.h>
#include <swap.h>
#include <vm.h>

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
coremap* cm_entry;
bool bootstrapped = false;
unsigned int totalpagecnt;
struct spinlock cm_lock;
paddr_t firstaddr;
uint32_t counter;

void
vm_bootstrap(void)
{
	paddr_t lastaddr, freeaddr, buf;

	ram_getsize(&firstaddr, &lastaddr);
	totalpagecnt = (unsigned int) (lastaddr - firstaddr) / PAGE_SIZE;

	cm_entry = (coremap*) PADDR_TO_KVADDR(firstaddr);
	freeaddr = firstaddr + totalpagecnt * sizeof(coremap);

	buf = firstaddr;
	for(unsigned int page = 0; page < totalpagecnt; page++){
		(cm_entry+page)->cm_addrspace = NULL;
		(cm_entry+page)->cm_vaddr = PADDR_TO_KVADDR(buf);
		(cm_entry+page)->cm_npages = 0;
		(cm_entry+page)->cm_timestamp = 0; 	

		if(buf < freeaddr){
			(cm_entry+page)->cm_state = FIXED;
			counter++;
		}else {
			(cm_entry+page)->cm_state = FREE;
		}

		buf += PAGE_SIZE;
	}

	bootstrapped = true;
	swapspace_init();	
	spinlock_init(&cm_lock);
}

int
get_page_count(vaddr_t address)
{
	coremap* temp = cm_entry;

	unsigned int count = 0;
	while((temp+count)->cm_vaddr != address){
		count++;
		if(count >= totalpagecnt){
			return 0;
		}
	}

	int result = (temp+count)->cm_npages;
	
	return result;
}

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	
	if(!bootstrapped){
		pa = getppages(npages);
		if (pa == 0) {
			return 0;
		}
		return PADDR_TO_KVADDR(pa);
	}else {
		return page_nalloc(npages);	
	}
}

unsigned int
get_total_page_count(void)
{
	return totalpagecnt;
}

paddr_t
get_first_page(void)
{
	return firstaddr;
}

void
set_swapin(struct addrspace* as, vaddr_t va){
	coremap* temp = cm_entry;
	for(unsigned int page = 0; page < totalpagecnt;page++){
		if((temp + page)->cm_addrspace == as && (temp + page)->cm_vaddr == va){
			(temp + page)->cm_state = SWAPPING;
			break;
		}
	}
}

void
revert_swapin(struct addrspace* as, vaddr_t va){
	coremap* temp = cm_entry;
	for(unsigned int page = 0; page < totalpagecnt;page++){
                if((temp + page)->cm_addrspace == as && (temp + page)->cm_vaddr == va){
                        (temp + page)->cm_state = DIRTY;
                        break;
                }
        }
}

void
delete_coremap(struct addrspace* as){
	spinlock_acquire(&cm_lock);

	coremap* temp = cm_entry;
	paddr_t buf = firstaddr;
	for(unsigned int page = 0; page < totalpagecnt;page++){
		if((temp + page)->cm_addrspace == as && (temp + page)->cm_state != FIXED){
			
			struct tlbshootdown tlb;
		        tlb.ts_addrspace = (temp + page)->cm_addrspace;
		        tlb.ts_vaddr = (temp + page)->cm_vaddr;
			vm_tlbshootdown(&tlb);
		        ipi_broadcast(IPI_TLBSHOOTDOWN);

			(temp + page)->cm_addrspace = NULL;
			(temp + page)->cm_state = FREE;
		}
		
		buf += PAGE_SIZE;
	}
	spinlock_release(&cm_lock);
}

void
page_alloc(struct addrspace* as, vaddr_t va, bool forstack)
{
	(void)forstack;
	bool ispagefree = false;
	unsigned int page;
	
	for(page = 0; page < totalpagecnt; page++){
		if((cm_entry + page)->cm_state == FREE){
			ispagefree = true;
			break;
		}
	}

	coremap* alloc = cm_entry;
	if(!ispagefree){
		page = make_page_avail(&alloc, 1);
	}else{
		alloc = cm_entry + page;
		bzero((int*)PADDR_TO_KVADDR(firstaddr + (page * PAGE_SIZE)), PAGE_SIZE);
	}

	pagetable* temp = as->as_pgtable;

	while(temp != NULL && temp->pg_vaddr != va){
		temp = (pagetable*) temp->pg_next;
	}
	
	if(temp == NULL){
		spinlock_release(&cm_lock);
		return;		//Error: vaddr not found
	}
	temp->pg_paddr = firstaddr + (page * PAGE_SIZE);

	alloc->cm_addrspace = as;
	alloc->cm_vaddr = va;
	
	alloc->cm_timestamp = ++counter;
	alloc->cm_state = DIRTY;
	alloc->cm_npages = 1;
}

vaddr_t
page_nalloc(int npages)
{
	int freepages = 0;
	unsigned int start = 0;
	spinlock_acquire(&cm_lock);

	for(unsigned int page = 0; page < totalpagecnt; page++){
		if((cm_entry + page)->cm_state == FREE){
			freepages = 1;
			for(int cont = 2; cont <= npages; cont++){
				if((cm_entry + page + cont)->cm_state == FREE){
					freepages++;
				}else{
					break;
				}
			}

			if(freepages == npages){
				start = page;
				break;
			}
			page += freepages - 1;
		}
	}

	coremap* allock = cm_entry;
	if(freepages != npages){
		start = make_page_avail(&allock, npages);
	}else {
		allock = cm_entry + start;
		bzero((int*)PADDR_TO_KVADDR(firstaddr + (start * PAGE_SIZE)), npages * PAGE_SIZE);
	}

	paddr_t paddr = firstaddr + (start * PAGE_SIZE);
	vaddr_t result = PADDR_TO_KVADDR(paddr);
	allock->cm_vaddr = result;

	for(int page = 0; page < npages; page++){
		(allock+page)->cm_state = FIXED;

		(allock+page)->cm_timestamp = ++counter;
	}
	
	allock->cm_npages = npages;
	spinlock_release(&cm_lock);
	return result;
}

/*Pick the oldest page available in the coremap so as to swap out*/
unsigned int
make_page_avail(coremap** temp, int npages)
{
	(void)npages;
	uint64_t oldertimestamp = 0;
    	unsigned int victimpage = 0;

	for(unsigned int page = 0; page < totalpagecnt; page++){
		if((oldertimestamp == 0) || (cm_entry + page)->cm_timestamp < oldertimestamp){
			if((cm_entry + page)->cm_state != FIXED && (cm_entry + page)->cm_state != SWAPPING){ 
				oldertimestamp = (cm_entry + page)->cm_timestamp;
	   	                victimpage = page;
			}
	        }
	}

	KASSERT(victimpage != 0);
	KASSERT((cm_entry + victimpage)->cm_addrspace != NULL);

	//Inform the caller about the index of coremap that is to be changed
        *temp = cm_entry + victimpage;

	pagetable* pg = (cm_entry + victimpage)->cm_addrspace->as_pgtable;
	while(pg != NULL){
		if(pg->pg_vaddr == (cm_entry + victimpage)->cm_vaddr){
			paddr_t tem = pg->pg_paddr;

			pg->pg_inswap = true;
			pg->pg_paddr = 0;
			pg->pg_inmem = false;

			struct tlbshootdown tlb;
			tlb.ts_addrspace = (cm_entry + victimpage)->cm_addrspace;
			tlb.ts_vaddr = (cm_entry + victimpage)->cm_vaddr;
		
			vm_tlbshootdown(&tlb);
			ipi_broadcast(IPI_TLBSHOOTDOWN);
	
			(cm_entry + victimpage)->cm_state = SWAPPING;
			swap_out((cm_entry + victimpage)->cm_addrspace, pg->pg_vaddr, (void*)PADDR_TO_KVADDR(tem));
			(cm_entry + victimpage)->cm_state = CLEAN;
			
			bzero((int*)PADDR_TO_KVADDR(tem), PAGE_SIZE);
			break;
		}

		pg = (pagetable*) pg->pg_next;
	}

	return victimpage;
}

/*Free the page allocated for kernel heap*/
void 
free_kpages(vaddr_t addr)
{
	spinlock_acquire(&cm_lock);
	
	for(unsigned int page = 0; page < totalpagecnt; page++){
		if((cm_entry + page)->cm_vaddr == addr){
			if((cm_entry + page)->cm_addrspace == NULL){
				coremap* temp = cm_entry;

				for(int npages = 0; npages < (cm_entry + page)->cm_npages; npages++){
					(temp + page + npages)->cm_state = FREE;
				}
				break;
			}
		}
	}
	spinlock_release(&cm_lock);
}

void
vm_tlbshootdown_all(void)
{
	int spl = splhigh();

	for(int cnt = 0; cnt < NUM_TLB; cnt++){
		tlb_write(TLBHI_INVALID(cnt), TLBLO_INVALID(), cnt);
	}

	splx(spl);
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	int spl = splhigh();
	
	int index = tlb_probe(ts->ts_vaddr, 0);

	if(index != -1){
		tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
	}

	splx(spl);
}

void
check_for_swap(vaddr_t va){
	coremap* temp = cm_entry;

	for(unsigned int page = 0; page < totalpagecnt; page++){
		if((temp + page)->cm_vaddr == va && (temp + page)->cm_addrspace == curthread->t_addrspace){
			while((temp + page)->cm_state == SWAPPING){
				spinlock_release(&cm_lock);
				thread_yield();
				spinlock_acquire(&cm_lock);
			}
			break;
		}
	}
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	uint32_t ehi, elo;	
	int spl;
	paddr_t paddr;
	bool invalidvaddr = true;

	faultaddress &= PAGE_FRAME;

	if(curthread->t_addrspace == NULL){
		return EFAULT;
	}

	spinlock_acquire(&cm_lock);
	switch (faulttype) {
		case VM_FAULT_READONLY:
			panic("Read only");
		break;
		case VM_FAULT_READ:
		case VM_FAULT_WRITE:
		{
			pagetable* table = curthread->t_addrspace->as_pgtable;
			
			while(table != NULL){
				if(table->pg_vaddr == faultaddress){
					check_for_swap(faultaddress);
					
					if(table->pg_paddr == 0){
						page_alloc(curthread->t_addrspace, faultaddress, false);
					
						if(table->pg_inmem == false){
							swap_in(curthread->t_addrspace, faultaddress, (void*)PADDR_TO_KVADDR(table->pg_paddr));
							table->pg_inmem = true;
						}
					}
					paddr = table->pg_paddr;
					invalidvaddr = false;
					break;
				}
				table = (pagetable*) table->pg_next;
			}

			if(invalidvaddr){
				spinlock_release(&cm_lock);
				return EFAULT;
			}
		}
		break;
		default:
			spinlock_release(&cm_lock);
			return EFAULT;
	}

	spl = splhigh();

	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	tlb_random(ehi, elo);

        splx(spl);
	spinlock_release(&cm_lock);
	return 0;
}
