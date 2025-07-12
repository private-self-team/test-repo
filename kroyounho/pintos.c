#include "devices/input.h"
#include "devices/kbd.h"
#include "devices/serial.h"
#include "devices/timer.h"
#include "devices/vga.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/thread.h"
#include <console.h>
#include <debug.h>
#include <limits.h>
#include <random.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef USERPROG
#include "userprog/exception.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#endif
#include "tests/threads/tests.h"
#ifdef VM
#include "vm/vm.h"
#endif
#ifdef FILESYS
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "filesys/fsutil.h"
#endif

uint64_t *base_pml4;

#ifdef FILESYS
static bool format_filesys;
#endif

bool power_off_when_done;
bool thread_tests;

static void bss_init(void);
static void paging_init(uint64_t mem_end);
static char **read_command_line(void);
static char **parse_options(char **argv);
static void run_actions(char **argv);
static void usage(void);
static void print_stats(void);
static void power_off(void); // static 처리 완료

int main(void) NO_RETURN;

int main(void) {
  uint64_t mem_end;
  char **argv;

  bss_init();
  argv = read_command_line();
  argv = parse_options(argv);

  thread_init();
  console_init();

  mem_end = palloc_init();
  malloc_init();
  paging_init(mem_end);

#ifdef USERPROG
  tss_init();
  gdt_init();
#endif

  intr_init();
  timer_init();
  kbd_init();
  input_init();
#ifdef USERPROG
  exception_init();
  syscall_init();
#endif
  thread_start();
  serial_init_queue();
  timer_calibrate();

#ifdef FILESYS
  disk_init();
  filesys_init(format_filesys);
#endif

#ifdef VM
  vm_init();
#endif

  printf("Boot complete.\n");

  run_actions(argv);

  if (power_off_when_done)
    power_off();
  thread_exit();
}

static void bss_init(void) {
  extern char _start_bss, _end_bss;
  memset(&_start_bss, 0,
         (uintptr_t)&_end_bss -
             (uintptr_t)&_start_bss); // 포인터 빼기 문제 해결
}

static void paging_init(uint64_t mem_end) {
  uint64_t *pml4;
  pml4 = base_pml4 = palloc_get_page(PAL_ASSERT | PAL_ZERO);

  extern char start, _end_kernel_text;

  for (uint64_t pa = 0; pa < mem_end; pa += PGSIZE) {
    uint64_t va = (uint64_t)ptov(pa);

    int perm = PTE_P | PTE_W;
    if ((uint64_t)&start <= va && va < (uint64_t)&_end_kernel_text)
      perm &= ~PTE_W;

    uint64_t *pte = pml4e_walk(pml4, va, 1);
    if (pte != NULL)
      *pte = pa | perm;
  }

  pml4_activate(0);
}

static char **read_command_line(void) {
  static char *argv[LOADER_ARGS_LEN / 2 + 1];
  char *p = ptov(LOADER_ARGS);
  const char *end =
      p + LOADER_ARGS_LEN; // cppcheck-suppress constVariablePointer
  int argc = *(uint32_t *)ptov(LOADER_ARG_CNT);
  int i;

  for (i = 0; i < argc; i++) {
    if (p >= end)
      PANIC("command line arguments overflow");

    argv[i] = p;
    p += strnlen(p, end - p) + 1;
  }
  argv[argc] = NULL;

  printf("Kernel command line:");
  for (i = 0; i < argc; i++)
    if (strchr(argv[i], ' ') == NULL)
      printf(" %s", argv[i]);
    else
      printf(" '%s'", argv[i]);
  printf("\n");

  return argv;
}

static char **parse_options(char **argv) {
  for (; *argv != NULL && **argv == '-'; argv++) {
    char *save_ptr;
    char *name = strtok_r(*argv, "=", &save_ptr);
    char *value = strtok_r(NULL, "", &save_ptr);

    if (!strcmp(name, "-h"))
      usage();
    else if (!strcmp(name, "-q"))
      power_off_when_done = true;
#ifdef FILESYS
    else if (!strcmp(name, "-f"))
      format_filesys = true;
#endif
    else if (!strcmp(name, "-rs"))
      random_init(atoi(value));
    else if (!strcmp(name, "-mlfqs"))
      thread_mlfqs = true;
#ifdef USERPROG
    else if (!strcmp(name, "-ul"))
      user_page_limit = atoi(value);
    else if (!strcmp(name, "-threads-tests"))
      thread_tests = true;
#endif
    else
      PANIC("unknown option `%s' (use -h for help)", name);
  }

  return argv;
}

static void run_task(char **argv) {
  const char *task = argv[1];

  printf("Executing '%s':\n", task);
#ifdef USERPROG
  if (thread_tests) {
    run_test(task);
  } else {
    process_wait(process_create_initd(task));
  }
#else
  run_test(task);
#endif
  printf("Execution of '%s' complete.\n", task);
}

static void run_actions(char **argv) {
  struct action {
    char *name;
    int argc;
    void (*function)(char **argv);
  };

  static const struct action actions[] = {
      {"run", 2, run_task},
#ifdef FILESYS
      {"ls", 1, fsutil_ls},   {"cat", 2, fsutil_cat}, {"rm", 2, fsutil_rm},
      {"put", 2, fsutil_put}, {"get", 2, fsutil_get},
#endif
      {NULL, 0, NULL},
  };

  while (*argv != NULL) {
    const struct action *a;
    int i;

    for (a = actions;; a++)
      if (a->name == NULL)
        PANIC("unknown action `%s' (use -h for help)", *argv);
      else if (!strcmp(*argv, a->name))
        break;

    for (i = 1; i < a->argc; i++)
      if (argv[i] == NULL)
        PANIC("action `%s' requires %d argument(s)", *argv, a->argc - 1);

    a->function(argv);
    argv += a->argc;
  }
}

static void usage(void) {
  printf("\nCommand line syntax: [OPTION...] [ACTION...]\n"
         "Options must precede actions.\n"
         "Actions are executed in the order specified.\n"
         "\nAvailable actions:\n"
#ifdef USERPROG
         "  run 'PROG [ARG...]' Run PROG and wait for it to complete.\n"
#else
         "  run TEST           Run TEST.\n"
#endif
#ifdef FILESYS
         "  ls                 List files in the root directory.\n"
         "  cat FILE           Print FILE to the console.\n"
         "  rm FILE            Delete FILE.\n"
         "Use these actions indirectly via `pintos' -g and -p options:\n"
         "  put FILE           Put FILE into file system from scratch disk.\n"
         "  get FILE           Get FILE from file system into scratch disk.\n"
#endif
         "\nOptions:\n"
         "  -h                 Print this help message and power off.\n"
         "  -q                 Power off VM after actions or on panic.\n"
         "  -f                 Format file system disk during startup.\n"
         "  -rs=SEED           Set random number seed to SEED.\n"
         "  -mlfqs             Use multi-level feedback queue scheduler.\n"
#ifdef USERPROG
         "  -ul=COUNT          Limit user memory to COUNT pages.\n"
#endif
  );
  power_off();
}

static void power_off(void) {
#ifdef FILESYS
  filesys_done();
#endif

  print_stats();

  printf("Powering off...\n");
  outw(0x604, 0x2000);
  for (;;)
    ;
}

static void print_stats(void) {
  timer_print_stats();
  thread_print_stats();
#ifdef FILESYS
  disk_print_stats();
#endif
  console_print_stats();
  kbd_print_stats();
#ifdef USERPROG
  exception_print_stats();
#endif
}
