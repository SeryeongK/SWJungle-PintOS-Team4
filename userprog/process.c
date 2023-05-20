#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);

/*-------------------------[project 2]-------------------------*/
void argument_stack(char **parse, int count, struct intr_frame *_if);
struct thread *get_child_process(int pid);
/*-------------------------[project 2]-------------------------*/

/* General process initializer for initd and other process. */
static void process_init(void)
{
    struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */

/*  ppt 상의 process_execute()함수
   새로운 initd 스레드를 생성하고, 생성한 스레드의 ID를 반환하는 함수 */
tid_t process_create_initd(const char *file_name)
{
    char *fn_copy;
    tid_t tid;
    char *save_ptr;

    /* Make a copy of FILE_NAME.
     * Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page(0);
    if (fn_copy == NULL)
        return TID_ERROR;
    strlcpy(fn_copy, file_name, PGSIZE);

    /* Create a new thread to execute FILE_NAME. */
    strtok_r(file_name, " ", &save_ptr);

    tid = thread_create(file_name, PRI_DEFAULT, initd, fn_copy);
    if (tid == TID_ERROR)
        palloc_free_page(fn_copy);
    return tid;
}

/* A thread function that launches first user process. */
static void initd(void *f_name)
{
#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif

    process_init();

    if (process_exec(f_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_ UNUSED)
{
    struct thread *curr = thread_current();
    memcpy(&curr->parent_if, if_, sizeof(struct intr_frame));

    tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, curr);
    /*return thread_create (name, PRI_DEFAULT, __do_fork, thread_current ());*/

    if (tid == TID_ERROR)
    {
        return TID_ERROR;
    }

    struct thread *child = get_child_process(tid);

    sema_down(&child->fork_sema);

    if (child->exit_status == -1)
        return TID_ERROR;

    return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool duplicate_pte(uint64_t *pte, void *va, void *aux)
{
    struct thread *current = thread_current();
    struct thread *parent = (struct thread *)aux;
    void *parent_page;
    void *newpage;
    bool writable;

    if (is_kernel_vaddr(va))
    {
        return true;
    }
    /* Resolve VA from the parent's page map level 4. */
    parent_page = pml4_get_page(parent->pml4, va);

    if (parent_page == NULL)
    {
        return false;
    }

    newpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (newpage == NULL)
    {
        return false;
    } /* set result to NEWPAGE*/

    // if (is_writable(pte)) {
    memcpy(newpage, parent_page, PGSIZE);
    writable = is_writable(pte);
    /* Add new page to child's page table at address VA with WRITABLE
     *    permission. */
    if (!pml4_set_page(current->pml4, va, newpage, writable))
    {
        printf("FAIL to insert page!\n");
        return false;
    }
    return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void __do_fork(void *aux)
{
    struct intr_frame if_;
    struct thread *parent = (struct thread *)aux;
    struct thread *current = thread_current();
    struct intr_frame *parent_if = &parent->parent_if; /* project2 수정*/

    bool succ = true;

    memcpy(&if_, parent_if, sizeof(struct intr_frame));

    if_.R.rax = 0; /* project2 추가*/

    /*  Duplicate PT */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL)
        goto error;

    process_activate(current);
#ifdef VM
    supplemental_page_table_init(&current->spt);
    if (!supplemental_page_table_copy(&current->spt, &parent->spt))
        goto error;
#else
    if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
        goto error;
#endif
    if (parent->next_fd >= FDCOUNT_LIMIT)
        goto error;

    /*-------------------------[project 2]-------------------------*/
    current->fdt[0] = parent->fdt[0];
    current->fdt[1] = parent->fdt[1];

    for (int i = 2; i < FDCOUNT_LIMIT; i++)
    {
        struct file *f = parent->fdt[i];
        if (f == NULL)
        {
            continue;
        }
        current->fdt[i] = file_duplicate(f);
    }

    current->next_fd = parent->next_fd;
    sema_up(&current->fork_sema);
    /*-------------------------[project 2]-------------------------*/

    /* Finally, switch to the newly created process. */
    if (succ)
        do_iret(&if_);
error:
    current->exit_status = TID_ERROR;
    sema_up(&current->fork_sema);
    exit(TID_ERROR);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
// ppt 상 start_process()
int process_exec(void *f_name)
{
    char *file_name = f_name;
    bool success;
    // memcpy(values, file_name, strlen(file_name) + 1);

    /* We cannot use the intr_frame in the thread structure.
     * This is because when current thread rescheduled,
     * it stores the execution information to the member. */
    struct intr_frame _if;
    _if.ds = _if.es = _if.ss = SEL_UDSEG;
    _if.cs = SEL_UCSEG;
    _if.eflags = FLAG_IF | FLAG_MBS;

    /* We first kill the current context */
    /*-------------------------[project 2]-------------------------*/
    char *token, *save_ptr;
    char *values[128];
    int i = 0;

    token = strtok_r(f_name, " ", &save_ptr);
    values[i] = token;

    while (token != NULL)
    {
        token = strtok_r(NULL, " ", &save_ptr);
        i++;
        values[i] = token;
    }
    /*-------------------------[project 2]-------------------------*/

    process_cleanup();

#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif
    /* And then load the binary */
    success = load(file_name, &_if);
    /* If load failed, quit. */
    /*-------------------------[project 2]-------------------------*/
    if (!success)
    {
        palloc_free_page(file_name);
        return -1;
    }
    argument_stack(values, i, &_if);
    /*-------------------------[project 2]-------------------------*/

    // hex_dump(_if.rsp, _if.rsp, USER_STACK - _if.rsp, true);
    /* Start switched process. */
    do_iret(&_if);
    NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */

/*-------------------------[project 2]-------------------------*/
/* 지정한 child_tid의 자식 프로세스가 종료되길 기다리고,
종료 시 자식 프로세스의 exit_status를 반환하는 함수 */
int process_wait(tid_t child_tid UNUSED)
{
    struct thread *child = get_child_process(child_tid);
    if (child == NULL)
    {
        return -1;
    }
    sema_down(&child->wait_sema);
    // thread_exit();
    int exit_status = child->exit_status;
    list_remove(&child->child_elem);
    sema_up(&child->free_sema);
    return exit_status;
}

/* 프로세스를 종료하는 함수 */
void process_exit(void)
{
    struct thread *curr = thread_current();

    for (int i = 0; i < FDCOUNT_LIMIT; i++)
    {
        close(i);
    }
    palloc_free_multiple(curr->fdt, FDT_PAGES);
    file_close(curr->running);

    sema_up(&curr->wait_sema);
    sema_down(&curr->free_sema);

    process_cleanup();
}

/* 현재 프로세스의 자원을 해제하는 함수 */
static void process_cleanup(void)
{
    struct thread *curr = thread_current();
#ifdef VM
    supplemental_page_table_kill(&curr->spt);
#endif

    uint64_t *pml4;
    /* Destroy the current process's page directory and switch back
     * to the kernel-only page directory. */
    pml4 = curr->pml4;
    if (pml4 != NULL)
    {
        /* Correct ordering here is crucial.  We must set
         * cur->pagedir to NULL before switching page directories,
         * so that a timer interrupt can't switch back to the
         * process page directory.  We must activate the base page
         * directory before destroying the process's page
         * directory, or our active page directory will be one
         * that's been freed (and cleared). */
        curr->pml4 = NULL;
        pml4_activate(NULL);
        pml4_destroy(pml4);
    }
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
    /* Activate thread's page tables. */
    pml4_activate(next->pml4);

    /* Set thread's kernel stack for use in processing interrupts. */
    tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load(const char *file_name, struct intr_frame *if_)
{
    struct thread *t = thread_current();
    struct ELF ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* Allocate and activate page directory. */
    t->pml4 = pml4_create();
    if (t->pml4 == NULL)
        goto done;
    process_activate(thread_current());

    /* Open executable file. */
    file = filesys_open(file_name);
    if (file == NULL)
    {
        printf("load: %s: open failed\n", file_name);
        goto done;
    }

    /* 실행 중인 스레드 t의 running을 실행할 파일로 초기화*/
    t->running = file;

    /* 현재 오픈한 파일에 다른내용 쓰지 못하게 함 */
    file_deny_write(file);

    /* Read and verify executable header. */
    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
        || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
    {
        printf("load: %s: error loading executable\n", file_name);
        goto done;
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++)
    {
        struct Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file))
            goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
            /* Ignore this segment. */
            break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            goto done;
        case PT_LOAD:
            if (validate_segment(&phdr, file))
            {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint64_t file_page = phdr.p_offset & ~PGMASK;
                uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint64_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0)
                {
                    /* Normal segment.
                     * Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
                }
                else
                {
                    /* Entirely zero.
                     * Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                }
                if (!load_segment(file, file_page, (void *)mem_page,
                                  read_bytes, zero_bytes, writable))
                    goto done;
            }
            else
                goto done;
            break;
        }
    }

    /* Set up stack. */
    if (!setup_stack(if_))
        goto done;

    /* Start address. */
    if_->rip = ehdr.e_entry;

    /* TODO: Your code goes here.
     * TODO: Implement argument passing (see project2/argument_passing.html). */

    success = true;

done:
    /* We arrive here whether the load is successful or not. */
    // file_close(file);
    return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
{
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (uint64_t)file_length(file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;

    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr((void *)phdr->p_vaddr))
        return false;
    if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* The region cannot "wrap around" across the kernel virtual
       address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* Disallow mapping page 0.
       Not only is it a bad idea to map page 0, but if we allowed
       it then user code that passed a null pointer to system calls
       could quite likely panic the kernel by way of null pointer
       assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* It's okay. */
    return true;
}
/*----------------week09 추가 함수--------------------*/

void argument_stack(char **parse, int count, struct intr_frame *_if)
{
    char *value_address[128];

    for (int i = count - 1; i >= 0; i--)
    {
        int parse_len = strlen(parse[i]);
        _if->rsp -= (parse_len + 1);
        memcpy(_if->rsp, parse[i], parse_len + 1);
        value_address[i] = _if->rsp;
    }

    while (_if->rsp % 8 != 0)
    {
        _if->rsp--;
        memset(_if->rsp, 0, sizeof(uint8_t));
    }

    for (int i = count; i >= 0; i--)
    {
        _if->rsp = _if->rsp - 8;

        if (i == count)
            memset(_if->rsp, 0, 8);
        else
        {
            memcpy(_if->rsp, &value_address[i], 8);
        }
    }
    /* fake address */
    _if->rsp -= 8;
    memset(_if->rsp, 0, 8);

    /* register*/
    _if->R.rdi = count;
    _if->R.rsi = _if->rsp + 8;
}

struct thread *get_child_process(int pid)
{
    struct thread *curr = thread_current();
    if (list_empty(&curr->children_list))
    {
        return NULL;
    }

    for (struct list_elem *e = list_begin(&curr->children_list); e != list_end(&curr->children_list); e = list_next(e))
    {
        struct thread *t = list_entry(e, struct thread, child_elem);
        if (t->tid == pid)
        {
            return t;
        }
    }
    return NULL;
}

// void remove_child_process(struct thread *cp)
// {
// 	list_remove(&cp->child_elem);
// 	free(cp);
// }
/*----------------week09 추가 함수 끝--------------------*/

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0)
    {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t *kpage = palloc_get_page(PAL_USER);
        if (kpage == NULL)
            return false;

        /* Load this page. */
        if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
        {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable))
        {
            printf("fail\n");
            palloc_free_page(kpage);
            return false;
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
/**
 *  사용자 프로세스의 스택을 설정
 * 스택은 프로세스가 함수 호출을 수행하거나 지역 변수를 저장할 때 사용하는 메모리 영역  🤬
 */
static bool
setup_stack(struct intr_frame *if_)
{
    uint8_t *kpage;
    bool success = false;
    /* 새로운 페이지를 할당 , PAL_USER는 사용자 모드 페이지를, PAL_ZERO는 페이지 내용 페이지 내용을 0으로 초기화하는 옵션 */
    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL)
    {
        /* 할당받은 페이지를 사용자 스택의 주소 공간에 매핑
        install_page 함수는 가상 주소에 실제 메모리 페이지를 매핑하는 함수 */
        success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
        if (success)
            if_->rsp = USER_STACK;
        else
            palloc_free_page(kpage);
    }
    return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
/* 주어진 가상 주소 upage에 대해 물리 페이지 kpage를 매핑하는 함수 */
static bool
install_page(void *upage, void *kpage, bool writable)
{
    struct thread *t = thread_current();

    /* Verify that there's not already a page at that virtual
     * address, then map our page there. */
    return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else

/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

bool install_page(void *upage, void *kpage, bool writable)
{
    struct thread *t = thread_current();

    /* Verify that there's not already a page at that virtual
     * address, then map our page there. */
    return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}

/* 실제로 해당 데이터가 필요한 시점에 데이터를 로드하는 함수 */
bool lazy_load_segment(struct page *page, void *aux)
{
    /* aux를 container 구조체로 변환하여 필요한 정보 추출 */
    struct file *file = ((struct container *)aux)->file;
    off_t offset = ((struct container *)aux)->offset;
    size_t read_bytes = ((struct container *)aux)->read_bytes;
    size_t size_for_zero = PGSIZE - read_bytes;

    /* 파일 포인터를 올바른 오프셋으로 이동 */
    file_seek(file, offset);

    if (file_read(file, page->frame->kva, read_bytes) != (int)read_bytes)
    {
        /* 읽은 바이트 수가 read_bytes와 일치하지 않으면, 페이지 할당을 해제 */
        palloc_free_page(page->frame->kva);
        return false;
    }
    /* 나머지 부분을 0으로 초기화 */
    memset(page->frame->kva + read_bytes, 0, size_for_zero);
    /* 파일 포인터의 위치를 다시 오프셋으로 이동
       🤔 왜 다음 페이지의 시작 지점이 아닌 로드한 페이지의 시작 지점으로 오프셋을 이동? */
    file_seek(file, offset);

    return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
/* 특정 파일의 특정 오프셋에서 시작하는 세그먼트를 로드하는 역할을 하는 함수 */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0); /* 오프셋이 페이지 크기로 정렬되어있는지 */

    file_seek(file, ofs);

    while (read_bytes > 0 || zero_bytes > 0)
    {
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE; /* 파일에서 읽을 바이트 수 */
        size_t page_zero_bytes = PGSIZE - page_read_bytes;                  /* 초기화 해야 할 바이트 수*/

        /* TODO: Set up aux to pass information to the lazy_load_segment. */
        struct lazy_load_info *aux = malloc(sizeof(struct lazy_load_info));
        aux->file = file;
        aux->ofs = ofs;
        aux->page_read_bytes = page_read_bytes;
        aux->page_zero_bytes = page_zero_bytes;
        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable, lazy_load_segment, aux))
        {
            free(aux);
            return false;
        }
        /* Advance. */
        read_bytes -= page_read_bytes; /* 다음 반복에서 읽어야 할 남은 바이트 수가 갱신 */
        zero_bytes -= page_zero_bytes; /* 다음 반복에서 0으로 채워야 할 남은 바이트 수가 갱신 */
        upage += PGSIZE;               /* PGSIZE 만큼 증가시켜 다음 페이지의 주소로 이동 */
        ofs += page_read_bytes;        /* ofs에 더해 다음에 읽어야 할 파일 위치를 갱신 */
    }
    return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack(struct intr_frame *if_)
{
    void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

    /* TODO: Map the stack on stack_bottom and claim the page immediately.
     * TODO: If success, set the rsp accordingly.
     * TODO: You should mark the page is stack. */
    /* TODO: Your code goes here */
    if (vm_alloc_page(VM_ANON | VM_MARKER_STACK, stack_bottom, 1))
    {
        if (vm_claim_page(stack_bottom))
        {
            if_->rsp = USER_STACK;
            thread_current()->stack_bottom = stack_bottom;
            return true;
        }
    }
    return false;
}

#endif /* VM */