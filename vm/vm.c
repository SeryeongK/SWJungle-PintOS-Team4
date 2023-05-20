/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/hash.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include <stdbool.h>
#include "userprog/process.h"

/*----------------[project3]-------------------*/
struct list frame_table;
struct list_elem *start;

static unsigned vm_hash_func(const struct hash_elem *e, void *aux);
static bool vm_less_func(const struct hash_elem *a, const struct hash_elem *b);
static void spt_destroy_func(struct hash_elem *e, void *aux);
/*----------------[project3]-------------------*/

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
	vm_anon_init();
	vm_file_init();
#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_table);
	start = list_begin(&frame_table);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

/*----------------[project3]-------------------*/
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);
void spt_dealloc(struct hash_elem *e, void *aux);

bool insert_page(struct hash *pages, struct page *p);
bool delete_page(struct hash *pages, struct page *p);
/*----------------[project3]-------------------*/

/* 페이지 할당 및 초기화하는 함수 */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux)
{
	ASSERT(VM_TYPE(type) != VM_UNINIT);
	struct supplemental_page_table *spt = &thread_current()->spt;

	/*----------------[project3]-------------------*/
	if (spt_find_page(spt, upage) == NULL)
	{
		/* upage를 spt에서 찾을 수 없다면 */
		struct page *new_page = malloc(sizeof(struct page));
		bool (*page_initializer)(struct page *, enum vm_type, void *kva);
		switch (VM_TYPE(type))
		{
		case VM_ANON:
			page_initializer = anon_initializer;
			break;
		case VM_FILE:
			page_initializer = file_backed_initializer;
			break;
		}
		uninit_new(new_page, upage, init, type, aux, page_initializer);

		new_page->writable = writable;
		new_page->t = thread_current(); /* 페이지의 소유 스레드 설정 */

		return spt_insert_page(spt, new_page);
	}
	/*----------------[project3]-------------------*/
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/* 주어진 spt에서 va(virtual address)에 해당하는 구조체 page를 찾는 함수 */
struct page *
spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED)
{
	struct page *page = (struct page *)malloc(sizeof(struct page));
	struct hash_elem *e = hash_find(&spt->hash_table, &page->hash_elem);

	page->va = pg_round_down(va);
	e = hash_find(&spt->hash_table, &page->hash_elem);
	free(page);

	return e != NULL ? hash_entry(e, struct page, hash_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED, struct page *page UNUSED)
{
	return insert_page(&spt->hash_table, page);
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	vm_dealloc_page(page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim(void)
{
	struct frame *victim = NULL;
	struct thread *curr = thread_current();
	struct list_elem *e = start;

	for (start = e; start != list_end(&frame_table); start = list_next(start))
	{
		victim = list_entry(start, struct frame, frame_elem);
		if (pml4_is_accessed(curr->pml4, victim->page->va))
			pml4_set_accessed(curr->pml4, victim->page->va, 0);
		else
			return victim;
	}

	for (start = list_begin(&frame_table); start != e; start = list_next(start))
	{
		victim = list_entry(start, struct frame, frame_elem);
		if (pml4_is_accessed(curr->pml4, victim->page->va))
			pml4_set_accessed(curr->pml4, victim->page->va, 0);
		else
			return victim;
	}

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void)
{
	struct frame *victim = vm_get_victim();
	/* TODO: swap out the victim and return the evicted frame. */

	swap_out(victim->page);

	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *vm_get_frame(void)
{
	struct frame *frame = (struct frame *)malloc(sizeof(struct frame));

	frame->kva = palloc_get_page(PAL_USER);
	if (frame->kva == NULL)
	{
		frame = vm_evict_frame();
		frame->page = NULL;

		return frame;
	}
	list_push_back(&frame_table, &frame->frame_elem);

	frame->page = NULL;

	ASSERT(frame != NULL);
	ASSERT(frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static bool
vm_stack_growth(void *addr UNUSED)
{
	if (vm_alloc_page(VM_ANON | VM_MARKER_0, addr, 1))
	{
		vm_claim_page(addr);
		thread_current()->stack_bottom -= PGSIZE;
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp(struct page *page UNUSED)
{
}

/* 페이지 폴트를 처리하는 시도를 수행하는 함수 */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED, bool write UNUSED, bool not_present UNUSED)
{
	struct supplemental_page_table *spt UNUSED = &thread_current()->spt;

	if (is_kernel_vaddr(addr))
	{
		return false;
	}

	void *rsp_stack = is_kernel_vaddr(f->rsp) ? thread_current()->rsp_stack : f->rsp;
	if (not_present)
	{
		if (!vm_claim_page(addr))
		{
			if (rsp_stack - 8 <= addr && USER_STACK - 0x100000 <= addr && addr <= USER_STACK)
			{
				vm_stack_growth(thread_current()->stack_bottom - PGSIZE);
				return true;
			}
			return false;
		}
		else
			return true;
	}
	return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
/* 주어진 가상 주소에 해당하는 페이지를 실제로 할당하고 관리하기 위한 함수 */
bool vm_claim_page(void *va UNUSED)
{
	struct page *page = spt_find_page(&thread_current()->spt, va); /* spt에서 va에 해당하는 페이지 찾기 */

	if (page == NULL)
	{
		return false;
	}

	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
/* 페이지를 확보하고, 페이지와 프레임 간의 링크를 설정하며, 페이지 테이블 엔트리를 삽입하는 함수 */
static bool
vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if (install_page(page->va, frame->kva, page->writable))
	{
		return swap_in(page, frame->kva);
	}
	return false;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
	hash_init(&spt->hash_table, vm_hash_func, vm_less_func, NULL);
}

/* Copy supplemental page table from src to dst */
/* src에서 dst로 spt을 복사하는 함수 */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
								  struct supplemental_page_table *src UNUSED)
{
	struct hash_iterator i;
	hash_first(&i, &src->hash_table);

	/* src의 각각의 페이지를 반복문을 통해 복사 */
	while (hash_next(&i))
	{
		struct page *parent_page = hash_entry(hash_cur(&i), struct page, hash_elem);
		enum vm_type type = page_get_type(parent_page);
		void *upage = parent_page->va;
		bool writable = parent_page->writable;
		vm_initializer *init = parent_page->uninit.init;
		void *aux = parent_page->uninit.aux;

		if (parent_page->operations->type == VM_UNINIT)
		{
			/* 부모 타입이 uninit인 경우 */
			if (!vm_alloc_page_with_initializer(type, upage, writable, init, aux)) /* 지정된 타입과 초기화 함수를 가진 페이지를 할당하고 초기화
																					  🤔 부모 페이지는 초기화되지 않았는데 왜 자식 페이지는 초기화하는가?
																						 자식 페이지가 부모 페이지의 내용을 정상적으로 상속받을 수 있도록 하기 위해서 */
				return false;
		}
		else
		{
			/* 부모 타입이 uninit이 아닌 경우 */
			if (!vm_alloc_page(type, upage, writable)) /* 지정된 타입으로 빈 페이지 할당 */
				return false;
			if (!vm_claim_page(upage)) /* 해당 페이지 확보 */
				return false;
			struct page *child_page = spt_find_page(dst, upage);
			memcpy(child_page->frame->kva, parent_page->frame->kva, PGSIZE);
		}
	}
	return true;
}

/* 보조 페이지 테이블이 보유한 리소스를 해제하는 함수 */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
	struct hash_iterator i;

	hash_first(&i, &spt->hash_table);
	while (hash_next(&i))
	{
		struct page *page = hash_entry(hash_cur(&i), struct page, hash_elem);

		if (page->operations->type == VM_FILE)
		{
			do_munmap(page->va);
		}
	}
	hash_destroy(&spt->hash_table, spt_destroy_func);
}

/*----------------[project3]-------------------*/
/* vm_entry의 vaddr을 인자값으로 hash_int() 함수를 사용하여 해시 값 반환 */
static unsigned vm_hash_func(const struct hash_elem *e, void *aux)
{
	const struct page *p = hash_entry(e, struct page, hash_elem);
	return hash_bytes(&p->va, sizeof p->va);
}

/* 입력된 두 hash_elem의 vaddr 비교
 a의 vaddr이 b보다 작을 시 true 반환
 a의 vaddr이 b보다 클 시 false 반환 */
static bool vm_less_func(const struct hash_elem *a, const struct hash_elem *b)
{
	/* hash_entry()로 각각의 element에 대한 vm_entry 구조체를 얻은 후
	vaddr 비교 (b가 크다면 true, a가 크다면 false */
	void *hash_A = hash_entry(a, struct page, hash_elem)->va;
	void *hash_B = hash_entry(b, struct page, hash_elem)->va;

	return (hash_A) < (hash_B);
}

bool insert_page(struct hash *pages, struct page *p)
{
	if (!hash_insert(pages, &p->hash_elem))
		return true;
	else
		return false;
}

bool delete_page(struct hash *pages, struct page *p)
{
	if (!hash_delete(pages, &p->hash_elem))
		return true;
	else
		return false;
}

static void spt_destroy_func(struct hash_elem *e, void *aux)
{
	const struct page *pg = hash_entry(e, struct page, hash_elem);
	vm_dealloc_page(pg);
}
/*----------------[project3]-------------------*/
