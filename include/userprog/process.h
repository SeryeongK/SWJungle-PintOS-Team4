#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd(const char *file_name);
tid_t process_fork(const char *name, struct intr_frame *if_ UNUSED);
int process_exec(void *f_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(struct thread *next);
/*-------------------------[project 2]-------------------------*/
void argument_stack(char **parse, int count, struct intr_frame *if_);
struct thread *get_child_process(int pid);
/*-------------------------[project 2]-------------------------*/
/*-------------------------[project 3]-------------------------*/
bool install_page(void *upage, void *kpage, bool writable);
bool lazy_load_segment(struct page *page, void *aux);
struct file *process_get_file(int fd);

struct container
{
    struct file *file;
    off_t offset;
    size_t read_bytes;
};

/*-------------------------[project 3]-------------------------*/

#endif /* userprog/process.h */
