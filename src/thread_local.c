
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
//__thread __attribute__ ((tls_model("initial-exec"))) uint64_t
// overflow_stack_space[1024];
//__thread __attribute__ ((tls_model("initial-exec"))) uint64_t* overflow_stack
//= NULL;

uint64_t overflow_stack_space[1024];
uint64_t* overflow_stack = &overflow_stack_space[0];

#define NUM_REG 16
#define REG_SIZE (256 / 64)
#define CONTEXT_SIZE (NUM_REG) * (REG_SIZE)
//__thread __attribute__ ((tls_model("initial-exec"))) uint64_t
// cfi_context[CONTEXT_SIZE];
//__thread __attribute__ ((tls_model("initial-exec"))) uint64_t
// user_context[CONTEXT_SIZE];

uint64_t cfi_context[CONTEXT_SIZE];
uint64_t user_context[CONTEXT_SIZE];

uint64_t n_overflow_pushes = 0;
uint64_t n_overflow_pops = 0;

/*
static void setup_memory(uint64_t** mem_ptr, long size) {
  // Add space for two guard pages at the beginning and the end of the stack
  int page_size = getpagesize();
  size += page_size * 2;

  *mem_ptr = (uint64_t*)mmap(0, size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  // Protect guard pages
  mprotect(*mem_ptr, page_size, PROT_NONE);
  mprotect(*mem_ptr + size - page_size, page_size, PROT_NONE);

  // Skip past the guard page at the beginning
  *mem_ptr += page_size;
}
*/
void litecfi_mem_initialize() {
  return;
  // long stack_size = pow(2, 16);  // Stack size = 2^16
  // setup_memory(&overflow_stack, stack_size);
}
void litecfi_overflow_stack_push() {
  if (overflow_stack == NULL) {
    overflow_stack = (uint64_t*)overflow_stack_space;
  }
  if (overflow_stack == (uint64_t*)overflow_stack_space) {
    asm("pextrq $1, %%xmm15, %%r11 \n\t"
        "lea 64(%%r11), %%r11 \n\t"
        "pinsrq $1, %%r11, %%xmm15 \n\t"
        :
        :
        :);
  }
  asm("movq (%%rdi), %%r10;\n\t"
      "movq %%r10, (%0);\n\t"
      "addq $8, %0;\n\t"
      : "+a"(overflow_stack)
      :
      :);
}

void litecfi_overflow_stack_pop() {
  uint64_t ret_addr;
  asm("subq $8, %0;\n\t"
      "movq (%0), %1;\n\t"
      : "+d"(overflow_stack), "=b"(ret_addr)
      :);
  asm goto("cmp %0, (%%rdi); \n\t"
           "jz %l1; \n\t"
           "int $3; \n\t"
           :
           : "b"(ret_addr)
           : "cc"
           : ok);
ok:
  if (overflow_stack == (uint64_t*)overflow_stack_space) {
    asm("pextrq $1,%%xmm15, %%r11 \n\t"
        "lea -64(%%r11), %%r11 \n\t"
        "pinsrq $1, %%r11, %%xmm15\n\t"
        :
        :
        :);
  }

  return;
}

// Overflow functions for v3 jump table implementation
//
// Overflow slots look like below. See codegen.cc for the actual contents of
// each of these slots.
//
//          push_n |         |
//                 |---------|
//           pop_n |         |
//                 |---------|  <--- Start of overflow stack
// overflow_push_0 |         |
//                 |---------|
//    overflow_pop |         |
//                 |---------|
// overflow_push_1 |         |  <--- Second overflow push slot
//
//
//
//  Notes:
//
//  1. At the entry to first overflow push the stack pointer will be have been
//  positioned (which happens at the call site) to second overflow push slot. If
//  there is a subsequent push this second push slot will be executed. Upon
//  entry to this second push slot the stack pointer will now be out of bounds
//  with the ponter bump at the call site. However within this second push slot
//  we first reposition the stack pointer to the second push overflow slot and
//  then transfer the control back to the first push slot to handle the actual
//  push to the overflow stack. This cycle repeats with all subsequent pushes.
//  Invariant here is that stack pointer is never seen out of bounds outside the
//  overflow push slots. Also note that the first overflow push slot will not
//  make any stack pointer adjustments within it. Hence the control flow
//  redirect from second push slot works as intended.
//
//  2. When a pop happens from the overflow stack, the pointer will always be at
//  the start of second overflow push slot at the pop function call site (due to
//  the invariant at 1). Then the pointer will be set to the start of first
//  overflow push slot at the call site and the pop slot entered. If at the end
//  of the pop we discover the overflow_stack is not yet empty we reposition the
//  stack pointer to the start of second overflow push slot. This will make next
//  pops to also go through the overflow pop slot. If however, the overflow
//  stack is empty we do nothing, since at the next pop, the call site will
//  decrement the pointer back to regular stack slots and a regular stack pop
//  will happen.
//
//  With this scheme we are able to handle the transitions between the overflow
//  and regular stacks seamlessly. However one disadvantage of this approach is
//  that each overflow push after the first one will incur an additional
//  indirect jump from the second overflow push slot to the first.

void litecfi_overflow_stack_push_v3() {
  if (overflow_stack == NULL) {
    overflow_stack = (uint64_t*)overflow_stack_space;
  }
  asm("movq (%%rdi), %%r10;\n\t"
      "movq %%r10, (%0);\n\t"
      "addq $8, %0;\n\t"
      : "+a"(overflow_stack)
      :
      :);
}

void litecfi_overflow_stack_pop_v3() {
  uint64_t ret_addr;

  asm("subq $8, %0;\n\t"
      "movq (%0), %1;\n\t"
      : "+d"(overflow_stack), "=b"(ret_addr)
      :);
  asm goto("cmp %0, (%%rdi); \n\t"
           "jz %l1; \n\t"
           "int $3; \n\t"
           :
           : "b"(ret_addr)
           : "cc"
           : ok);
ok:
  if (overflow_stack > (uint64_t*)overflow_stack_space) {
    asm("vmovq %%xmm15, %%r11 \n\t"
        "lea 64(%%r11), %%r11 \n\t"
        "vmovq %%r11, %%xmm15 \n\t"
        :
        :
        :);
  }

  return;
}

// Overflow functions which includes stat collection

void litecfi_overflow_stack_push_v3_stats() {
  if (overflow_stack == NULL) {
    overflow_stack = (uint64_t*)overflow_stack_space;
  }

  __atomic_add_fetch(&n_overflow_pushes, 1, __ATOMIC_SEQ_CST);

  asm("movq (%%rdi), %%r10;\n\t"
      "movq %%r10, (%0);\n\t"
      "addq $8, %0;\n\t"
      : "+a"(overflow_stack)
      :
      :);
}

void litecfi_overflow_stack_pop_v3_stats() {
  uint64_t ret_addr;

  __atomic_add_fetch(&n_overflow_pops, 1, __ATOMIC_SEQ_CST);

  asm("subq $8, %0;\n\t"
      "movq (%0), %1;\n\t"
      : "+d"(overflow_stack), "=b"(ret_addr)
      :);
  asm goto("cmp %0, (%%rdi); \n\t"
           "jz %l1; \n\t"
           "int $3; \n\t"
           :
           : "b"(ret_addr)
           : "cc"
           : ok);
ok:
  if (overflow_stack > (uint64_t*)overflow_stack_space) {
    asm("vmovq %%xmm15, %%r11 \n\t"
        "lea 64(%%r11), %%r11 \n\t"
        "vmovq %%r11, %%xmm15 \n\t"
        :
        :
        :);
  }

  return;
}

void litecfi_stack_print_stats() {
  printf("[Statistics] Number of overflow pushes : %lu\n", n_overflow_pushes);
  printf("[Statistics] Number of overflow pops : %lu\n", n_overflow_pops);
  printf("[Statistics] Total overflow operations  : %lu\n",
         n_overflow_pushes + n_overflow_pops);
}

#define VECTOR_REGISTER_OP(mask, index, mem, insn)                             \
  if (mask & (1U << index)) {                                                  \
    asm(insn : : "r"(&mem[index * REG_SIZE]) :);                               \
  }

void litecfi_register_spill(unsigned mask) {
  VECTOR_REGISTER_OP(mask, 8, cfi_context, "vmovdqu %%ymm8, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 9, cfi_context, "vmovdqu %%ymm9, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 10, cfi_context, "vmovdqu %%ymm10, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 11, cfi_context, "vmovdqu %%ymm11, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 12, cfi_context, "vmovdqu %%ymm12, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 13, cfi_context, "vmovdqu %%ymm13, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 14, cfi_context, "vmovdqu %%ymm14, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 15, cfi_context, "vmovdqu %%ymm15, (%0);\n\t");
}

void litecfi_register_restore(unsigned mask) {
  VECTOR_REGISTER_OP(mask, 8, cfi_context, "vmovdqu (%0), %%ymm8;\n\t");
  VECTOR_REGISTER_OP(mask, 9, cfi_context, "vmovdqu (%0), %%ymm9;\n\t");
  VECTOR_REGISTER_OP(mask, 10, cfi_context, "vmovdqu (%0), %%ymm10;\n\t");
  VECTOR_REGISTER_OP(mask, 11, cfi_context, "vmovdqu (%0), %%ymm11;\n\t");
  VECTOR_REGISTER_OP(mask, 12, cfi_context, "vmovdqu (%0), %%ymm12;\n\t");
  VECTOR_REGISTER_OP(mask, 13, cfi_context, "vmovdqu (%0), %%ymm13;\n\t");
  VECTOR_REGISTER_OP(mask, 14, cfi_context, "vmovdqu (%0), %%ymm14;\n\t");
  VECTOR_REGISTER_OP(mask, 15, cfi_context, "vmovdqu (%0), %%ymm15;\n\t");
}

void litecfi_ctx_save(unsigned mask) {
  VECTOR_REGISTER_OP(mask, 8, user_context, "vmovdqu %%ymm8, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 9, user_context, "vmovdqu %%ymm9, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 10, user_context, "vmovdqu %%ymm10, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 11, user_context, "vmovdqu %%ymm11, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 12, user_context, "vmovdqu %%ymm12, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 13, user_context, "vmovdqu %%ymm13, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 14, user_context, "vmovdqu %%ymm14, (%0);\n\t");
  VECTOR_REGISTER_OP(mask, 15, user_context, "vmovdqu %%ymm15, (%0);\n\t");
}

void litecfi_ctx_restore(unsigned mask) {
  VECTOR_REGISTER_OP(mask, 8, user_context, "vmovdqu (%0), %%ymm8;\n\t");
  VECTOR_REGISTER_OP(mask, 9, user_context, "vmovdqu (%0), %%ymm9;\n\t");
  VECTOR_REGISTER_OP(mask, 10, user_context, "vmovdqu (%0), %%ymm10;\n\t");
  VECTOR_REGISTER_OP(mask, 11, user_context, "vmovdqu (%0), %%ymm11;\n\t");
  VECTOR_REGISTER_OP(mask, 12, user_context, "vmovdqu (%0), %%ymm12;\n\t");
  VECTOR_REGISTER_OP(mask, 13, user_context, "vmovdqu (%0), %%ymm13;\n\t");
  VECTOR_REGISTER_OP(mask, 14, user_context, "vmovdqu (%0), %%ymm14;\n\t");
  VECTOR_REGISTER_OP(mask, 15, user_context, "vmovdqu (%0), %%ymm15;\n\t");
}
