/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/synch.h"
#include "lib/kernel/bitmap.h"
#include "threads/vaddr.h"
#include <string.h>

#define SECTOR_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)
static struct bitmap *swap_bitmap;
static struct lock swap_lock;

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	swap_disk = disk_get(1, 1);
	swap_bitmap = bitmap_create(disk_size(swap_disk)/SECTOR_PER_PAGE);
	bitmap_set_all(swap_bitmap, false);
	lock_init(&swap_lock);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	return true;

	struct anon_page *anon_page = &page->anon;
	anon_page->swap_idx = BITMAP_ERROR;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;

	if(anon_page->swap_idx == BITMAP_ERROR){
		memset(kva, 0, PGSIZE);
		return true;
	}

	lock_acquire(&swap_lock);

	for(int i = 0; i<SECTOR_PER_PAGE; i++){
		disk_read(swap_bitmap, anon_page->swap_idx*SECTOR_PER_PAGE + i, kva + i*DISK_SECTOR_SIZE);
	}
	bitmap_reset(swap_bitmap, anon_page->swap_idx);
	anon_page->swap_idx = BITMAP_ERROR;

	lock_release(&swap_lock);
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	lock_acquire(&swap_lock);

	size_t idx = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
	if(idx == BITMAP_ERROR){
		lock_release(&swap_lock);
		return false;
	}

	for(int i = 0; i<SECTOR_PER_PAGE; i++){
		disk_write(swap_disk, idx*SECTOR_PER_PAGE + i, page->frame->kva + i*DISK_SECTOR_SIZE);
	}
	anon_page->swap_idx = idx;

	lock_release(&swap_lock);
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	if(anon_page->swap_idx != BITMAP_ERROR){
		lock_acquire(&swap_lock);
		bitmap_reset(swap_bitmap, anon_page->swap_idx);
		lock_release(&swap_lock);
	}

	if(page->frame != NULL){

		list_remove(&(page->spt_elem));

		palloc_free_page(page->frame);
		free(page->frame);
		page->frame = NULL;
	}
}
