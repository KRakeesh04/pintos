#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "filesys/file.h"
#include <list.h>

/* File descriptor structure. */
struct file_descriptor
{
  int fd;                /* File descriptor number. */
  struct file *file;     /* File pointer. */
  struct list_elem elem; /* List element. */
};

void syscall_init(void);
void exit_process(int status);

#endif /* userprog/syscall.h */
