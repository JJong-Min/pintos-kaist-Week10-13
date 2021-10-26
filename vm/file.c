/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "vm/file.h"
#include <string.h>
#include "threads/malloc.h"
#include "threads/mmu.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};
//Record of mmap, store mmap_file_info
static struct list mmap_file_list;

struct mmap_file_info{
	struct list_elem elem;
	uint64_t start;
	// start addr of final page
	uint64_t end;
};

/* The initializer of file vm */
void
vm_file_init (void) {
	// list_init (&mmap_file_list);
}


/* Initialize the file mapped page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	struct file* file = ((struct mmap_info*)page ->uninit.aux)->file;
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
	file_page -> file = file;
	return true;
}


/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	// TODO: On mmap_exit sometimes empty file content
	struct file_page *file_page = &page->file;
	//if dirty, write back to file
	if (pml4_is_dirty (thread_current() -> pml4, page -> va)){
		file_seek (file_page->file, file_page->ofs);
		file_write (file_page->file, page->va, file_page->size);
	}
	file_close (file_page->file);

	if (page->frame != NULL) {
		list_remove (&page->frame->elem);
		free (page->frame);
	}
}

//Use to lazy load mmap
static bool
lazy_load_file (struct page* page, void* aux){
	struct mmap_info* mi = (struct mmap_info*) aux;
	file_seek (mi->file, mi->offset);
	page -> file.size = file_read (mi->file, page->va, mi->read_bytes);
	page -> file.ofs = mi->offset;
	if (page->file.size != PGSIZE){
		memset (page->va + page->file.size, 0, PGSIZE - page->file.size);
	}
	pml4_set_dirty (thread_current()->pml4, page->va, false);
	free(mi);
	return true;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	//assume all parameter errors are handled in syscall.c
	off_t ofs;
	uint64_t read_bytes;
	for (uint64_t i = 0; i < length; i += PGSIZE){
		struct mmap_info* mi = malloc (sizeof (struct mmap_info));
		ofs = offset + i;
		read_bytes = length - i >= PGSIZE ? PGSIZE : length -i;
		mi->file = file_reopen (file);
		mi->offset = ofs;
		mi->read_bytes = read_bytes;
		vm_alloc_page_with_initializer (VM_FILE, (void*) ((uint64_t) addr + i), writable, lazy_load_file, (void*) mi);
	}
	struct mmap_file_info* mfi = malloc (sizeof (struct mmap_file_info));
	mfi->start = (uint64_t) addr;
	mfi->end = (uint64_t) pg_round_down((uint64_t) addr + length -1);
	list_push_back(&mmap_file_list, &mfi->elem);
	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
}
