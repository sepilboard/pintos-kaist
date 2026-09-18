/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/thread.h"

#include <string.h>

#define STACK_MAX (1<<20)

struct list frame_table;
struct lock frame_table_lock;

void vm_free_frame(struct frame *frame);

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_table);
	lock_init(&frame_table_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	if (spt_find_page (spt, upage) == NULL) {
		bool (*page_initializer)(struct page *, enum vm_type, void *) = NULL;
		switch(VM_TYPE(type)){
			case VM_ANON:
				page_initializer = anon_initializer;
				break;

			case VM_FILE:
				page_initializer = file_backed_initializer;
				break;

			default:
				return false;
		}

		struct page *page = malloc(sizeof *page);
		if(page == NULL) return false;

		uninit_new(page, upage, init, type, aux, page_initializer);
		page->origin_writable = writable;
		page->writable = writable;
		page->pml4 = thread_current()->pml4;

		if(!spt_insert_page(spt, page)){
			vm_dealloc_page(page);
			return false;
		}

		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	
	void *target = pg_round_down(va);
	for(struct list_elem *e = list_begin(&(spt->pages));
	e != list_end(&(spt->pages));
	e = list_next(e)){
		page = list_entry(e, struct page, spt_elem);
		if(page->va == target) return page;
	}

	return NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	
	if(spt_find_page(spt, page->va) != NULL){
		return false;
	}

	list_push_back(&spt->pages, &page->spt_elem);
	return true;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	list_remove (&page->spt_elem);
	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	if(!list_empty(&frame_table)){
		struct list_elem *victim_elem = list_pop_front(&frame_table);
		victim = list_entry(victim_elem, struct frame, frame_elem);
		list_push_back(&frame_table, victim_elem);
	}
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	if(victim == NULL) return NULL;

	struct page *page = victim->page;
	/* TODO: swap out the victim and return the evicted frame. */
	swap_out(page);

	lock_acquire(&frame_table_lock);

	pml4_clear_page(page->pml4, page->va);
	page->frame = NULL;
	victim->page = NULL;

	lock_release(&frame_table_lock);

	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame;
	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);

	if(kva == NULL)
		return vm_evict_frame();

	frame = malloc(sizeof *frame);
	if(frame == NULL){
		palloc_free_page(kva);
		return NULL;
	}

	frame->kva = kva;
	frame->page = NULL;

	lock_acquire(&frame_table_lock);
	list_push_back(&frame_table, &frame->frame_elem);
	lock_release(&frame_table_lock);

	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
	void *page = pg_round_down(addr);
	vm_alloc_page(VM_ANON | VM_MARKER_0, page, 1);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page) {
	struct frame *frame = page->frame;
	if(!page->origin_writable){
		return false;
	}

	if(frame->ref_cnt == 1){
		pml4_set_page(page->pml4, page->va, frame->kva, true);
		page->writable = page->origin_writable;
		return true;
	}

	
	struct frame *new_frame = vm_get_frame();
	if(new_frame == NULL) return false;
	
	memcpy(new_frame->kva, frame->kva, PGSIZE);
	
	frame->ref_cnt--;
	new_frame->page = page;
	new_frame->ref_cnt = 1;
	
	pml4_set_page(page->pml4, page->va, new_frame->kva, true);
	// printf("ok\n");

	page->writable = true;
	page->frame = new_frame;

	return true;
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr,
		bool user UNUSED, bool write, bool not_present UNUSED) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;

	void *stack_pointer = f->rsp;
	if(stack_pointer-8 == addr
		&& USER_STACK - STACK_MAX <= stack_pointer-8 && stack_pointer-8 < USER_STACK){
		vm_stack_growth(addr);
	}

	page = spt_find_page(spt, addr);

	if(page == NULL){
		return false;
	}

	if(write && !page->writable){
		return vm_handle_wp(page);
	}

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct supplemental_page_table *spt = &(thread_current()->spt);
	struct page *page = spt_find_page(spt, va);
	if(page == NULL) return false;
	if(page->frame != NULL) return true;

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	if(frame == NULL) return false;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	if(!pml4_set_page(thread_current()->pml4,
						page->va,
						frame->kva,
						page->writable)){
		page->frame = NULL;
		frame->page = NULL;
		vm_free_frame(frame);
		return false;
	}

	if(!swap_in(page, frame->kva)){
		pml4_clear_page(thread_current()->pml4, page->va);

		page->frame = NULL;
		frame->page = NULL;
		vm_free_frame(frame);
		return false;
	}

	frame->ref_cnt = 1;

	return true;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	list_init(&(spt->pages));
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	for(struct list_elem *e = list_begin(&src->pages);
	e != list_end(&src->pages); e = list_next(e)){
		struct page *src_page = list_entry(e, struct page, spt_elem);
		// enum vm_type type = page_get_type(src_page);

		if(VM_TYPE(src_page->operations->type) == VM_UNINIT){
			vm_initializer *init = src_page->uninit.init;
			void *aux = src_page->uninit.aux;

			// printf("ok\n");

			vm_alloc_page_with_initializer(src_page->uninit.type, src_page->va, src_page->writable, init, aux);
		}
		else{
			struct page *child_page = (struct page *)malloc(sizeof(struct page));
			memcpy(child_page, src_page, sizeof(struct page));
			child_page->pml4 = thread_current()->pml4;
			
			pml4_set_page(src_page->pml4, src_page->va, src_page->frame->kva, false);
			pml4_set_page(child_page->pml4, src_page->va, src_page->frame->kva, false);
			src_page->writable = false;
			child_page->writable = false;
			spt_insert_page(dst, child_page);
	
			src_page->frame->ref_cnt++;
		}
	}

	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	while(!list_empty(&spt->pages)){
		struct list_elem *e = list_pop_front(&spt->pages);
		struct page *page = list_entry(e, struct page, spt_elem);

		vm_dealloc_page(page);
	}
}

void vm_free_frame(struct frame *frame)
{
	if(frame == NULL) return;

	lock_acquire(&frame_table_lock);
	list_remove(&frame->frame_elem);
	lock_release(&frame_table_lock);

	palloc_free_page(frame->kva);
	free(frame);
}