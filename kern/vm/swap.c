
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <lib.h>
#include <kern/fcntl.h>
#include <uio.h>
#include <kern/stat.h>
#include <vnode.h>
#include <vfs.h>
#include <vm.h>
#include <swap.h>

struct vnode* sw_vn;
extern struct spinlock cm_lock;

swapspace sw_space[MAX_VAL];

void
swapspace_init(){

	for(int itr = 0; itr < MAX_VAL; itr++){
		sw_space[itr].sw_vaddr = 0;
	}
	
	int ret = vfs_open((char*)"lhd0raw:", O_RDWR, 0664, &sw_vn);
	KASSERT(ret == 0);

}


int
read_page(void* kbuf, off_t sw_offset){
	struct iovec iovectr;
	struct uio uiovar;
	uio_kinit(&iovectr, &uiovar, kbuf, PAGE_SIZE, sw_offset, UIO_READ);
	int ret = VOP_READ(sw_vn, &uiovar);
	if(ret){
		return ret;
	}
//	*sw_offset = uio.uio_offset;
	return 0;
}

int
write_page(void* kbuf, off_t* newoffset){
	struct iovec iovectr;
	struct uio uiovar;
	struct stat st;

	if(sw_vn == NULL){
		return 1;	// not sure of the error, but an error code shud be returned
	}

	VOP_STAT(sw_vn,&st);	// gets the size of the file, so that we can append data at the end
	off_t offset = st.st_size;
	uio_kinit(&iovectr, &uiovar, kbuf, PAGE_SIZE, offset, UIO_WRITE);
	int ret = VOP_WRITE(sw_vn,&uiovar);

	if(ret){
		return 1;
	}

	*newoffset = offset;
	
	return 0;
}



int
swap_in(struct addrspace* as, vaddr_t va, void* kbuf){
	int itr =0;
	
	spinlock_acquire(&cm_lock);
	for(; itr < MAX_VAL; itr++){
                if(sw_space[itr].sw_addrspace == as && sw_space[itr].sw_vaddr == va){
			if(read_page(kbuf,sw_space[itr].sw_offset)){
				spinlock_release(&cm_lock);
				return 1;
			}
                        break;
                }
        }
	
	if(itr == MAX_VAL){
		spinlock_release(&cm_lock);
		return 1;
	}else{
		spinlock_release(&cm_lock);
		return 0;
	}
}


int
swap_out(struct addrspace* as, vaddr_t va, void* kbuf){
	
	int itr = 0;

	for(; itr < MAX_VAL; itr++){
		if(sw_space[itr].sw_vaddr==0){
			sw_space[itr].sw_addrspace = as;
			sw_space[itr].sw_vaddr = va;
			
			if(write_page(kbuf, &sw_space[itr].sw_offset)){
				return 1;
			}
			
			break;
		}
	}
	
	if(itr == MAX_VAL){
		return -1;
	}else{
		return 0;
	}
}