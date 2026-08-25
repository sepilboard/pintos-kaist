#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "threads/init.h"
#include "userprog/process.h"
#include "threads/palloc.h"
#include <string.h>

#include "filesys/file.h"
#include "filesys/filesys.h"

static struct lock filesys_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static void syscall_halt(void);
static void syscall_exec(const char *cmd_line);
static tid_t syscall_fork(const char *user_name, struct intr_frame *f);
static void syscall_exit(int status);

static bool syscall_create(const char *, unsigned);
static bool syscall_remove(const char *);
static int syscall_open(const char *);
static int syscall_filesize(int);
static int syscall_read(int, const void *, unsigned);
static int syscall_write(int, const void *, unsigned);
static void syscall_seek(int, unsigned);
static unsigned syscall_tell(int);
static void syscall_close(int);

static void check_user_address(const void *addr)
{
	struct thread *cur = thread_current();

	if(addr == NULL || !is_user_vaddr(addr)){
		syscall_exit(-1);
	}
}

static void check_user_buffer(const void *buffer, size_t size, bool writable)
{
	if(size == 0) return;

	const uint8_t *start = buffer;
	const uint8_t *end = start + size - 1;

	if(end<start) syscall_exit(-1);

	struct thread *cur = thread_current();

	for(const uint8_t *p = start; p<=end; p = pg_round_down(p) + PGSIZE){
		check_user_address(p);

		if(writable){
			uint64_t *pte = pml4e_walk(cur->pml4, (uint64_t) p, false);
			if(pte == NULL || !is_writable(pte)) syscall_exit(-1);
		}

		if(pg_round_down(p) == pg_round_down(end)) break;
	}
}

static char *copy_in_string(const char *user_string)
{
	char *copy = palloc_get_page(0);

	if(copy == NULL) syscall_exit(-1);

	for(int i = 0; i<PGSIZE; i++){
		check_user_address(user_string + i);
		copy[i] = user_string[i];

		if(copy[i] == '\0') return copy;
	}

	palloc_free_page(copy);
	syscall_exit(-1);
	return NULL;
}

static struct file* get_file(int fd)
{
	if(fd<2 || FD_COUNT<=fd) return NULL;

	return thread_current()->fd_table[fd];
}

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	switch(f->R.rax){
		case SYS_HALT:
			syscall_halt();
			break;

		case SYS_EXIT:
			syscall_exit(f->R.rdi);
			break;

		case SYS_EXEC:
			syscall_exec((const char *)f->R.rdi);
			break;

		case SYS_WAIT:
			f->R.rax = process_wait(f->R.rdi);
            break;

		case SYS_FORK:
			f->R.rax = syscall_fork((const char*)f->R.rdi, f);
			break;
		
		case SYS_CREATE:
			f->R.rax = syscall_create(f->R.rdi, f->R.rsi);
			break;

		case SYS_REMOVE:
			f->R.rax = syscall_remove(f->R.rdi);
			break;
		
		case SYS_OPEN:
			f->R.rax = syscall_open(f->R.rdi);
			break;
		
		case SYS_FILESIZE:
			f->R.rax = syscall_filesize(f->R.rdi);
			break;

		case SYS_READ:
			f->R.rax = syscall_read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		
		case SYS_WRITE:
			f->R.rax = syscall_write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;

		case SYS_SEEK:
			syscall_seek(f->R.rdi, f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = syscall_tell(f->R.rdi);
			break;

		case SYS_CLOSE:
			syscall_close(f->R.rdi);
			break;

		default:
			printf("Unknown system call: %lld\n", (long long)f->R.rax);
			thread_exit();
			break;
	}
}

static void syscall_halt(void){ power_off(); }
static void syscall_exit(int status)
{
    struct thread *cur = thread_current();

    cur->exit_status = status;
    thread_exit();
}
static tid_t syscall_fork(const char *user_name, struct intr_frame *f)
{
	char *name = copy_in_string(user_name);
	tid_t tid = process_fork(name, f);
	palloc_free_page(name);
	return tid;
}
static void syscall_exec(const char *cmd_line){
    // char *cmd_copy;

    // cmd_copy = palloc_get_page(0);
    // if(cmd_copy == NULL){
	// 	// syscall_exit(-1);
	// 	return -1;
	// }

    // strlcpy(cmd_copy, cmd_line, PGSIZE);

    // if(process_exec(cmd_copy) == -1){
	// 	// syscall_exit(-1);
	// 	return -1;
	// }
	char *cmd_copy = copy_in_string(cmd_line);

	if(process_exec(cmd_copy) < 0) syscall_exit(-1);
}

static bool syscall_create(const char *user_file, unsigned initial_size)
{
	char *file = copy_in_string(user_file);
	
	lock_acquire(&filesys_lock);
	bool ret = filesys_create(file, initial_size);
	lock_release(&filesys_lock);

	palloc_free_page(file);
	return ret;
}

static bool syscall_remove(const char* user_file)
{
	char *file = copy_in_string(user_file);

	lock_acquire(&filesys_lock);
	bool ret = filesys_remove(file);
	lock_release(&filesys_lock);

	palloc_free_page(file);
	return ret;
}

static int syscall_open(const char* user_file)
{
	char *file_name = copy_in_string(user_file);


	lock_acquire(&filesys_lock);
	struct file* file = filesys_open(file_name);
	palloc_free_page(file_name);
	
	if(file == NULL) return -1;
	
	struct thread *cur = thread_current();
	int ret = -1;
	for(int i = 2; i<FD_COUNT; i++){
		if(cur->fd_table[i] == NULL){
			cur->fd_table[i] = file;
			ret = i;
			break;
		}
	}

	if(ret == -1){
		file_close(file);
	}

	lock_release(&filesys_lock);

	return ret;
}

static int syscall_filesize(int fd)
{
	struct file *file = get_file(fd);
	if(file == NULL) return -1;

	lock_acquire(&filesys_lock);
	int ret = file_length(file);
	lock_release(&filesys_lock);

	return ret;
}

static int syscall_read(int fd, const void *buffer, unsigned size)
{
	check_user_buffer(buffer, size, true);
	
	struct file *file = get_file(fd);
	if(file == NULL) return -1;

	lock_acquire(&filesys_lock);
	int ret = file_read(file, buffer, size);
	lock_release(&filesys_lock);

	return ret;
}

static int syscall_write(int fd, const void *buffer, unsigned size)
{
	check_user_buffer(buffer, size, false);
	
	if(fd == 1){
		putbuf (buffer, size);
		return size;
	}

	struct file *file = get_file(fd);
	if(file == NULL) return -1;

	lock_acquire(&filesys_lock);
	int ret = file_write(file, buffer, size);
	lock_release(&filesys_lock);
	
	return ret;
}

static void syscall_seek(int fd, unsigned position)
{
	struct file* file = get_file(fd);
	if(file == NULL) return;

	lock_acquire(&filesys_lock);
	file_seek(file, position);
	lock_release(&filesys_lock);
}

static unsigned syscall_tell(int fd)
{
	struct file* file = get_file(fd);
	if(file == NULL) return -1;

	lock_acquire(&filesys_lock);
	unsigned ret = file_tell(file);
	lock_release(&filesys_lock);

	return ret;
}

static void syscall_close(int fd)
{
	if(fd< 2 || FD_COUNT<=fd) return;

	struct thread* cur = thread_current();
	struct file *file = cur->fd_table[fd];

	if(file == NULL) return;

	cur->fd_table[fd] = NULL;
	lock_acquire(&filesys_lock);
	file_close(file);
	lock_release(&filesys_lock);
}
