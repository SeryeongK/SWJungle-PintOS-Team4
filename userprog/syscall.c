#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
/*-------------------------[project 2]-------------------------*/
#include "filesys/filesys.h"
#include "userprog/process.h"
#include "devices/input.h"
#include "threads/palloc.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
void check_address(const void *addr);

void halt(void);
void exit(int status);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int filesize(int fd);
int exec(const char *cmd_line);
int open(const char *file);
int read(int fd, void *buffer, unsigned size);
int write(int fd, const void *buffer, unsigned size);
void seek(int fd, unsigned position);
void close(int fd);
tid_t fork(const char *thread_name, struct intr_frame *f);
int wait(tid_t pid);
unsigned tell(int fd);

struct file *process_get_file(int fd);
void process_close_file(int fd);
int process_add_file(struct file *f);
struct file *process_get_file(int fd);
/*-------------------------[project 2]-------------------------*/

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

const int STDIN = 1;
const int STDOUT = 2;

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	/* project2 */
	lock_init(&filesys_lock);
	/* project2 */
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f)
{
	switch (f->R.rax) // rax값이 들어가야함.
	{
	case SYS_HALT:
		halt();
		break;
	case SYS_EXIT:
		exit(f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = fork(f->R.rdi, f);
		break;
	case SYS_EXEC:
		if (exec(f->R.rdi) == -1)
		{
			exit(-1);
		}
		break;
	case SYS_WAIT:
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ:
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:
		seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = tell(f->R.rdi);
		break;
	case SYS_CLOSE:
		close(f->R.rdi);
		break;
	// case SYS_DUP2:
	// 	dup2(f->R.rdi, f->R.rsi);
	// 	break;
	// case SYS_MMAP:
	// 	mmap(f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
	// 	break;
	// case SYS_MUNMAP:
	// 	munmap(f->R.rdi);
	// 	break;
	// case SYS_CHDIR:
	// 	chdir(f->R.rdi);
	// 	break;
	// case SYS_MKDIR:
	// 	mkdir(f->R.rdi);
	// 	break;
	// case SYS_READDIR:
	// 	readdir(f->R.rdi, f->R.rsi);
	// 	break;
	// case SYS_ISDIR:
	// 	isdir(f->R.rdi);
	// 	break;
	// case SYS_INUMBER:
	// 	inumber(f->R.rdi);
	// 	break;
	// case SYS_SYMLINK:
	// 	symlink(f->R.rdi, f->R.rsi);
	// 	break;
	// case SYS_MOUNT:
	// 	mount(f->R.rdi, f->R.rsi, f->R.rdx);
	// 	break;
	// case SYS_UMOUNT:
	// 	umount(f->R.rdi);
	// 	break;
	default:
		exit(-1);
		break;
	}
}

/* 입력된 주소가 유효한 주소인지 확인하고, 그렇지 않으면 프로세스를 종료시키는 함수 */
void check_address(const void *addr)
{
	struct thread *curr = thread_current();

	if (is_kernel_vaddr(addr) || addr == NULL || pml4_get_page(curr->pml4, addr) == NULL)
	{
		exit(-1);
	}
}

// void get_argument(void *rsp, int **arg, int count)
// {
// 	rsp = (int64_t *)rsp + 2; // 원래 stack pointer에서 2칸(16byte) 올라감 : |argc|"argv"|...
// 	for (int i = 0; i < count; i++)
// 	{
// 		check_address(rsp);
// 		arg[i] = rsp;
// 		rsp = (int64_t *)rsp + 1;
// 	}
// }

/* pintos를 shutdown하는 시스템콜 함수  */
void halt(void)
{
	power_off();
}

/* process를 종료하는 시스템콜 함수 */
void exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;
	printf("%s: exit(%d)\n", thread_name(), status);
	thread_exit(); 
}

/* 'function함수를 수행하는 스레드'를 생성하는 시스템콜 함수 */
bool create(const char *file, unsigned initial_size)
{
	check_address(file);
	if (filesys_create(file, initial_size))
	{
		return true;
	}
	else
	{
		return false;
	}
}

/* 주어진 파일을 삭제하는 시스템콜 함수 */
bool remove(const char *file)
{
	check_address(file);

	if (filesys_remove(file))
	{
		return true;
	}
	return false;
}

/* 입력된  fd가 가르키는 파일의 크기를 반환하는 시스템콜 함수 */
int filesize(int fd)
{
	struct file *fileobj = process_get_file(fd);

	if (fileobj == NULL)
		return -1;

	return file_length(fileobj);
}

/* 자식프로세스를 생성하고 프로그램을 실행시키는 시스템콜 함수 */
int exec(const char *cmd_line)
{
	check_address(cmd_line);

	int size = strlen(cmd_line) + 1;
	char *fn_copy = palloc_get_page(PAL_ZERO);
	if (fn_copy == NULL)
		exit(-1);
	strlcpy(fn_copy, cmd_line, size);

	if (process_exec(fn_copy) == -1)
		return -1;

	NOT_REACHED();

	return 0;
}

/* 인자로 받은 file을 열어, 해당 파일을 가리키는 포인터를 현재 쓰레드의 fdt에 추가하는 시스템콜 함수 */
int open(const char *file)
{
	check_address(file);
	lock_acquire(&filesys_lock);
	struct file *fileobj = filesys_open(file);

	if (fileobj == NULL)
	{
		return -1;
	}
	int fd = process_add_file(fileobj);

	if (fd == -1)
	{
		file_close(fileobj);
	}

	lock_release(&filesys_lock);
	return fd;
}

/* 열린파일의 데이터를 읽는 시스템콜 함수*/
int read(int fd, void *buffer, unsigned size)
{
	check_address(buffer);	
	check_address(buffer + size - 1); 

	unsigned char *buf = buffer;
	int read_count;
	struct file *fileobj = process_get_file(fd);

	if (fileobj == NULL)
	{
		return -1;
	}

	if (size == 0)
	{
		return 0;
	}

	if (fileobj == STDIN)
	{
		char key;
		for (int read_count = 0; read_count < size; read_count++)
		{
			key = input_getc();
			*buf++ = key;
			if (key == '\0')
			{ 
				break;
			}
		}
	}
	else if (fileobj == STDOUT)
	{
		return -1;
	}
	else
	{
		lock_acquire(&filesys_lock);
		read_count = file_read(fileobj, buffer, size);
		lock_release(&filesys_lock);
	}
	return read_count;
}

/* 열린파일의 데이터를 기록하는 시스템콜 함수 */
int write(int fd, const void *buffer, unsigned size)
{
	check_address(buffer);

	int write_count;
	struct file *fileobj = process_get_file(fd);
	
	if (fileobj == NULL)
	{
		return -1;
	}

	if (fileobj == STDOUT)
	{
		putbuf(buffer, size);
		write_count = size;
	}
	else if (fileobj == STDIN)
	{
		return -1;
	}
	else
	{
		lock_acquire(&filesys_lock);
		write_count = file_write(fileobj, buffer, size);
		lock_release(&filesys_lock);
	}
	return write_count;
}

/* 열린 파일의 위치(offset)를 이동하는 시스템콜 함수*/
void seek(int fd, unsigned position)
{
	struct file *fileobj = process_get_file(fd);
	if (fd < 2)
	{
		return;
	}

	file_seek(fileobj, position);
}

/* 열린 파일의 위치(offset)를 알려주는 시스템콜 함수*/
unsigned tell(int fd)
{
	if (fd < 2)
	{
		return;
	}
	struct file *fileobj = process_get_file(fd);
	if (fd < 2)
	{
		return;
	}

	return file_tell(fileobj);
}

/* 열린 파일을 닫는 시스템 콜 함수*/
void close(int fd)
{
	if (fd <= 1)
		return;
	struct file *fileobj = process_get_file(fd);

	if (fileobj == NULL)
	{
		return;
	}
	process_close_file(fd);
}

/* 자식스레드를 복제하는 함수 */
tid_t fork(const char *thread_name, struct intr_frame *f)
{
	return process_fork(thread_name, f);
}

/* 지정된 pid를 가진 자식 프로세스가 종료될 때까지 대기하는 시스템콜 함수 */
int wait(tid_t pid)
{
	return process_wait(pid);
}

/*  현재 스레드의 fdt에 주어진 파일을 추가하고, 추가된 파일의 식별자를 반환하는 함수*/
int process_add_file(struct file *f)
{
	struct thread *curr = thread_current();
	struct file **fdt = curr->fdt;

	while (curr->next_fd < FDCOUNT_LIMIT && fdt[curr->next_fd])
	{
		curr->next_fd++;
	}

	if (curr->next_fd >= FDCOUNT_LIMIT)
		{
			return -1;
		}

	fdt[curr->next_fd] = f;
	return curr->next_fd;
}

/* 주어진 파일 식별자에 해당하는 파일 포인터를 반환하는 함수*/
struct file *process_get_file(int fd)
{
	if (fd < 0 || fd >= FDCOUNT_LIMIT || fd == NULL)
	{
		return NULL;
	}
	struct thread *curr = thread_current();
	return curr->fdt[fd];
}

/* 현재 실행중인 스레드의 fdt에서 fd 인덱스의 값을 NULL로 초기화하여 파일을 닫는 함수 */
void process_close_file(int fd)
{
	struct thread *curr = thread_current();
	if (fd < 0 || fd >= FDCOUNT_LIMIT)
		return;

	curr->fdt[fd] = NULL;
}