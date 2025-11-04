#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

static void syscall_handler(struct intr_frame *);
static struct lock filesys_lock;

/* User memory access functions. */
static int get_user(const uint8_t *uaddr);
static bool put_user(uint8_t *udst, uint8_t byte);
static void check_user_ptr(const void *ptr);
static void check_user_buffer(const void *buffer, unsigned size, bool writable);
static void *get_arg(struct intr_frame *f, int offset);

/* System call implementations. */
static void syscall_halt(struct intr_frame *f);
static void syscall_exit(struct intr_frame *f);
static void syscall_exec(struct intr_frame *f);
static void syscall_wait(struct intr_frame *f);
static void syscall_create(struct intr_frame *f);
static void syscall_remove(struct intr_frame *f);
static void syscall_open(struct intr_frame *f);
static void syscall_filesize(struct intr_frame *f);
static void syscall_read(struct intr_frame *f);
static void syscall_write(struct intr_frame *f);
static void syscall_seek(struct intr_frame *f);
static void syscall_tell(struct intr_frame *f);
static void syscall_close(struct intr_frame *f);

/* File descriptor helper functions. */
static struct file_descriptor *get_fd(int fd);
static void close_all_fds(void);

void syscall_init(void)
{
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}

/* Reads a byte at user virtual address UADDR.
   Returns the byte value if successful, -1 if a segfault occurred. */
static int
get_user(const uint8_t *uaddr)
{
  if (!is_user_vaddr(uaddr))
    return -1;

  int result;
  asm("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a"(result) : "m"(*uaddr));
  return result;
}

/* Writes BYTE to user address UDST.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user(uint8_t *udst, uint8_t byte)
{
  if (!is_user_vaddr(udst))
    return false;

  int error_code;
  asm("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a"(error_code), "=m"(*udst) : "q"(byte));
  return error_code != -1;
}

/* Checks if a user pointer is valid. */
static void
check_user_ptr(const void *ptr)
{
  if (ptr == NULL || !is_user_vaddr(ptr) ||
      get_user((const uint8_t *)ptr) == -1)
  {
    exit_process(-1);
  }
}

/* Checks if a user string is valid. */
static void
check_user_string(const char *str)
{
  if (str == NULL)
    exit_process(-1);

  /* Validate each byte of the string until null terminator. */
  const char *ptr = str;
  while (true)
  {
    int byte = get_user((const uint8_t *)ptr);
    if (!is_user_vaddr(ptr) || byte == -1)
      exit_process(-1);
    if (byte == 0)
      break;
    ptr++;
  }
}

/* Checks if a user buffer is valid. */
static void
check_user_buffer(const void *buffer, unsigned size, bool writable UNUSED)
{
  unsigned i;
  const char *buf = (const char *)buffer;

  for (i = 0; i < size; i++)
    check_user_ptr((const void *)(buf + i));
}

/* Gets a system call argument from the stack. */
static void *
get_arg(struct intr_frame *f, int offset)
{
  uint8_t *ptr = (uint8_t *)(f->esp + offset);

  /* Read 4 bytes using get_user to ensure safe access. */
  uint32_t result = 0;
  for (int i = 0; i < 4; i++)
  {
    if (!is_user_vaddr(ptr + i))
      exit_process(-1);
    int byte = get_user(ptr + i);
    if (byte == -1)
      exit_process(-1);
    result |= ((uint32_t)byte << (i * 8));
  }

  return (void *)result;
}

static void
syscall_handler(struct intr_frame *f)
{
  check_user_ptr((const void *)f->esp);
  int syscall_num = *(int *)f->esp;

  switch (syscall_num)
  {
  case SYS_HALT:
    syscall_halt(f);
    break;
  case SYS_EXIT:
    syscall_exit(f);
    break;
  case SYS_EXEC:
    syscall_exec(f);
    break;
  case SYS_WAIT:
    syscall_wait(f);
    break;
  case SYS_CREATE:
    syscall_create(f);
    break;
  case SYS_REMOVE:
    syscall_remove(f);
    break;
  case SYS_OPEN:
    syscall_open(f);
    break;
  case SYS_FILESIZE:
    syscall_filesize(f);
    break;
  case SYS_READ:
    syscall_read(f);
    break;
  case SYS_WRITE:
    syscall_write(f);
    break;
  case SYS_SEEK:
    syscall_seek(f);
    break;
  case SYS_TELL:
    syscall_tell(f);
    break;
  case SYS_CLOSE:
    syscall_close(f);
    break;
  default:
    exit_process(-1);
    break;
  }
}

/* Terminates Pintos. */
static void
syscall_halt(struct intr_frame *f UNUSED)
{
  shutdown_power_off();
}

/* Terminates the current user program. */
static void
syscall_exit(struct intr_frame *f)
{
  int status = (int)get_arg(f, 4);
  exit_process(status);
}

/* Executes a program. */
static void
syscall_exec(struct intr_frame *f)
{
  const char *cmd_line = (const char *)get_arg(f, 4);
  check_user_string(cmd_line);

  lock_acquire(&filesys_lock);
  f->eax = (uint32_t)process_execute(cmd_line);
  lock_release(&filesys_lock);
}

/* Waits for a child process. */
static void
syscall_wait(struct intr_frame *f)
{
  tid_t child_tid = (tid_t)get_arg(f, 4);
  f->eax = (uint32_t)process_wait(child_tid);
}

/* Creates a file. */
static void
syscall_create(struct intr_frame *f)
{
  const char *file = (const char *)get_arg(f, 4);
  unsigned initial_size = (unsigned)get_arg(f, 8);

  check_user_string(file);

  lock_acquire(&filesys_lock);
  f->eax = filesys_create(file, initial_size);
  lock_release(&filesys_lock);
}

/* Removes a file. */
static void
syscall_remove(struct intr_frame *f)
{
  const char *file = (const char *)get_arg(f, 4);
  check_user_string(file);

  lock_acquire(&filesys_lock);
  f->eax = filesys_remove(file);
  lock_release(&filesys_lock);
}

/* Opens a file. */
static void
syscall_open(struct intr_frame *f)
{
  const char *file_name = (const char *)get_arg(f, 4);
  check_user_string(file_name);

  lock_acquire(&filesys_lock);
  struct file *file = filesys_open(file_name);
  lock_release(&filesys_lock);

  if (file == NULL)
  {
    f->eax = -1;
    return;
  }

  struct file_descriptor *fd_entry = malloc(sizeof(struct file_descriptor));
  if (fd_entry == NULL)
  {
    lock_acquire(&filesys_lock);
    file_close(file);
    lock_release(&filesys_lock);
    f->eax = -1;
    return;
  }

  struct thread *cur = thread_current();
  fd_entry->fd = cur->next_fd++;
  fd_entry->file = file;
  list_push_back(&cur->fd_table, &fd_entry->elem);

  f->eax = fd_entry->fd;
}

/* Returns the size of a file. */
static void
syscall_filesize(struct intr_frame *f)
{
  int fd = (int)get_arg(f, 4);
  struct file_descriptor *fd_entry = get_fd(fd);

  if (fd_entry == NULL)
  {
    f->eax = -1;
    return;
  }

  lock_acquire(&filesys_lock);
  f->eax = file_length(fd_entry->file);
  lock_release(&filesys_lock);
}

/* Reads from a file. */
static void
syscall_read(struct intr_frame *f)
{
  int fd = (int)get_arg(f, 4);
  void *buffer = get_arg(f, 8);
  unsigned size = (unsigned)get_arg(f, 12);

  check_user_buffer(buffer, size, true);

  if (fd == 0)
  {
    /* Read from stdin. */
    unsigned i;
    uint8_t *buf = (uint8_t *)buffer;
    for (i = 0; i < size; i++)
      buf[i] = input_getc();
    f->eax = size;
    return;
  }

  struct file_descriptor *fd_entry = get_fd(fd);
  if (fd_entry == NULL)
  {
    f->eax = -1;
    return;
  }

  lock_acquire(&filesys_lock);
  f->eax = file_read(fd_entry->file, buffer, size);
  lock_release(&filesys_lock);
}

/* Writes to a file. */
static void
syscall_write(struct intr_frame *f)
{
  int fd = (int)get_arg(f, 4);
  const void *buffer = get_arg(f, 8);
  unsigned size = (unsigned)get_arg(f, 12);

  check_user_buffer(buffer, size, false);

  if (fd == 1)
  {
    /* Write to stdout. */
    putbuf(buffer, size);
    f->eax = size;
    return;
  }

  struct file_descriptor *fd_entry = get_fd(fd);
  if (fd_entry == NULL)
  {
    f->eax = 0;
    return;
  }

  lock_acquire(&filesys_lock);
  f->eax = file_write(fd_entry->file, buffer, size);
  lock_release(&filesys_lock);
}

/* Sets file position. */
static void
syscall_seek(struct intr_frame *f)
{
  int fd = (int)get_arg(f, 4);
  unsigned position = (unsigned)get_arg(f, 8);

  struct file_descriptor *fd_entry = get_fd(fd);
  if (fd_entry == NULL)
    return;

  lock_acquire(&filesys_lock);
  file_seek(fd_entry->file, position);
  lock_release(&filesys_lock);
}

/* Returns file position. */
static void
syscall_tell(struct intr_frame *f)
{
  int fd = (int)get_arg(f, 4);
  struct file_descriptor *fd_entry = get_fd(fd);

  if (fd_entry == NULL)
  {
    f->eax = -1;
    return;
  }

  lock_acquire(&filesys_lock);
  f->eax = file_tell(fd_entry->file);
  lock_release(&filesys_lock);
}

/* Closes a file. */
static void
syscall_close(struct intr_frame *f)
{
  int fd = (int)get_arg(f, 4);
  struct file_descriptor *fd_entry = get_fd(fd);

  if (fd_entry == NULL)
    return;

  lock_acquire(&filesys_lock);
  file_close(fd_entry->file);
  lock_release(&filesys_lock);

  list_remove(&fd_entry->elem);
  free(fd_entry);
}

/* Gets a file descriptor from the current thread's fd table. */
static struct file_descriptor *
get_fd(int fd)
{
  struct thread *cur = thread_current();
  struct list_elem *e;

  for (e = list_begin(&cur->fd_table); e != list_end(&cur->fd_table);
       e = list_next(e))
  {
    struct file_descriptor *fd_entry = list_entry(e, struct file_descriptor, elem);
    if (fd_entry->fd == fd)
      return fd_entry;
  }

  return NULL;
}

/* Closes all file descriptors for the current thread. */
static void
close_all_fds(void)
{
  struct thread *cur = thread_current();
  struct list_elem *e;

  while (!list_empty(&cur->fd_table))
  {
    e = list_pop_front(&cur->fd_table);
    struct file_descriptor *fd_entry = list_entry(e, struct file_descriptor, elem);

    lock_acquire(&filesys_lock);
    file_close(fd_entry->file);
    lock_release(&filesys_lock);

    free(fd_entry);
  }
}

/* Exits the current process with the given status. */
void exit_process(int status)
{
  struct thread *cur = thread_current();

  /* Set exit status in child_status structure. */
  if (cur->my_status != NULL)
    cur->my_status->exit_status = status;

  printf("%s: exit(%d)\n", cur->name, status);
  thread_exit();
}
