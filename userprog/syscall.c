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

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	printf("[syscall_handler] start : %lld, (%lld, %lld, %lld, %lld, %lld, %lld)\n", 
		f->R.rax, f->R.rdi,f->R.rsi,f->R.rdx,f->R.r10,f->R.r8,f->R.r9);
	switch(f->R.rax) {
		case SYS_HALT:                   /* Halt the operating system. */
			printf("  SYS_HALT called!\n");
			power_off ();
			break;

		case SYS_EXIT:				  	 /* Terminate this process. */
			printf("  SYS_EXIT called!\n");
			break;

		case SYS_FORK:                   /* Clone current process. */
			printf("  SYS_FORK called!\n");
			break;

		case SYS_EXEC:                   /* Switch current process. */
			printf("  SYS_EXEC called!\n");
			break;

		case SYS_WAIT:                   /* Wait for a child process to die. */
			printf("  SYS_WAIT called!\n");
			break;

		case SYS_CREATE:                 /* Create a file. */
			printf("  SYS_CREATE called!\n");
			break;

		case SYS_REMOVE:                 /* Delete a file. */
			printf("  SYS_REMOVE called!\n");
			break;

		case SYS_OPEN:                   /* Open a file. */
			printf("  SYS_OPEN called!\n");
			break;

		case SYS_FILESIZE:               /* Obtain a file's size. */
			printf("  SYS_FILESIZE called!\n");
			break;

		case SYS_READ:                   /* Read from a file. */
			printf("  SYS_READ called!\n");
			break;

		case SYS_WRITE:                  /* Write to a file. */
			printf("  SYS_WRITE called!\n");
			// power_off ();
			break;

		case SYS_SEEK:                   /* Change position in a file. */
			printf("  SYS_HALT called!\n");
			break;

		case SYS_TELL:                   /* Report current position in a file. */
			printf("  SYS_TELL called!\n");
			break;

		case SYS_CLOSE:                  /* Close a file. */
			printf("  SYS_CLOSE called!\n");
			break;

		default:
			printf("  DEFAULT do nothing..\n");
	}


	printf("[syscall_handler] end   : %lld \n", f->R.rax);

	thread_exit ();
}
