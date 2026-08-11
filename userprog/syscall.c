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

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static void syscall_halt(void);
static void syscall_exec(const char *cmd_line);
static tid_t syscall_fork(const char *user_name, struct intr_frame *f);
static void syscall_exit(int status);
static int syscall_write(int fd, const void *buffer, unsigned size);

static void
check_user_address(const void *addr)
{
	struct thread *cur = thread_current();

	if(addr == NULL ||
		!is_user_vaddr(addr) ||
		pml4_get_page(cur->pml4, addr) == NULL)
		syscall_exit(-1);
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

		case SYS_WRITE:
      	f->R.rax = syscall_write (
          	(int) f->R.rdi,
          	(const void *) f->R.rsi,
        	(unsigned) f->R.rdx
      	);
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
static int syscall_write(int fd, const void *buffer, unsigned size)
{
	if(fd == 1){
		putbuf (buffer, size);
		return size;
	}

	return -1;
}
