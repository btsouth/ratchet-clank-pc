/* libSceFiber: user-mode fibers on the calling thread, ported from shadPS4's
 * libraries/fiber (fiber.cpp, fiber_context.cpp). The guest's OrbisFiber layout is kept. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) && defined(__x86_64__)
#define FIBER_NULL ((int32_t)0x80590001)
#define FIBER_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_RANGE ((int32_t)0x80590003)
#define FIBER_INVALID ((int32_t)0x80590004)
#define FIBER_PERMISSION ((int32_t)0x80590005)
#define FIBER_STATE ((int32_t)0x80590006)
enum { RUN=1, IDLE=2, TERMINATED=3 };
enum { SET_FPU=0x100, MIN_CONTEXT=512 };
static const uint32_t SIGNATURE0=0xdef1649c, SIGNATURE1=0xb37592a0, OPT_SIGNATURE=0xbb40e64d;
static const uint64_t STACK_SIGNATURE=0x7149f2ca7149f2caull;
typedef void (ABI *FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);
struct Fiber;
typedef struct {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
    uint16_t fpucw;
    uint32_t mxcsr;
    struct Fiber *current_fiber, *prev_fiber;
    uint64_t arg_on_run_to, arg_on_return, return_val;
} FiberContext;
typedef struct {
    FiberEntry entry;
    uint64_t arg_on_initialize, arg_on_run_to;
    void *stack_addr;
    uint32_t *state;
    uint16_t fpucw;
    int8_t pad[2];
    uint32_t mxcsr;
} FiberData;
typedef struct Fiber {
    uint32_t magic_start;
    uint32_t state;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[32];
    FiberContext *context;
    uint32_t flags;
    void *context_start, *context_end;
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber)<=256,"OrbisFiber is at most 256 bytes");
_Static_assert(__builtin_offsetof(FiberContext,fpucw)==0x70,"context layout");
_Static_assert(__builtin_offsetof(FiberData,state)==0x20 && __builtin_offsetof(FiberData,fpucw)==0x28,"data layout");

int32_t bb_fiber_setjmp(FiberContext *ctx) __asm__("bb_fiber_setjmp");
void bb_fiber_longjmp(FiberContext *ctx) __asm__("bb_fiber_longjmp") __attribute__((noreturn));
void bb_fiber_switch_entry(FiberData *data, int set_fpu) __asm__("bb_fiber_switch_entry") __attribute__((noreturn));
__asm__(".text\n"
".globl bb_fiber_setjmp\nbb_fiber_setjmp:\n"
"    movq %rax, 0x0(%rdi)\n    movq (%rsp), %rdx\n    movq %rdx, 0x10(%rdi)\n"
"    movq %rcx, 0x08(%rdi)\n    movq %rbx, 0x18(%rdi)\n    movq %rsp, 0x20(%rdi)\n    movq %rbp, 0x28(%rdi)\n"
"    movq %r8, 0x30(%rdi)\n    movq %r9, 0x38(%rdi)\n    movq %r10, 0x40(%rdi)\n    movq %r11, 0x48(%rdi)\n"
"    movq %r12, 0x50(%rdi)\n    movq %r13, 0x58(%rdi)\n    movq %r14, 0x60(%rdi)\n    movq %r15, 0x68(%rdi)\n"
"    fnstcw 0x70(%rdi)\n    stmxcsr 0x72(%rdi)\n    xor %eax, %eax\n    ret\n"
".globl bb_fiber_longjmp\nbb_fiber_longjmp:\n"
"    stmxcsr -0x4(%rsp)\n    movl 0x72(%rdi), %eax\n    andl $0xffffffc0, %eax\n    movl -0x4(%rsp), %ecx\n"
"    andl $0x3f, %ecx\n    xorl %eax, %ecx\n    movl %ecx, -0x4(%rsp)\n    ldmxcsr -0x4(%rsp)\n"
"    movq 0x00(%rdi), %rax\n    movq 0x08(%rdi), %rcx\n    movq 0x10(%rdi), %rdx\n    movq 0x18(%rdi), %rbx\n"
"    movq 0x20(%rdi), %rsp\n    movq 0x28(%rdi), %rbp\n    movq 0x30(%rdi), %r8\n    movq 0x38(%rdi), %r9\n"
"    movq 0x40(%rdi), %r10\n    movq 0x48(%rdi), %r11\n    movq 0x50(%rdi), %r12\n    movq 0x58(%rdi), %r13\n"
"    movq 0x60(%rdi), %r14\n    movq 0x68(%rdi), %r15\n    fldcw 0x70(%rdi)\n"
"    movq %rdx, 0x00(%rsp)\n    movl $0x1, %eax\n    ret\n"
".globl bb_fiber_switch_entry\nbb_fiber_switch_entry:\n"
"    mov %rdi, %r11\n    movq 0x18(%r11), %rsp\n    xorl %ebp, %ebp\n    movq 0x20(%r11), %r10\n"
"    test %r10, %r10\n    jz 1f\n    movl $2, (%r10)\n"
"1:  test %esi, %esi\n    jz 2f\n    ldmxcsr 0x2c(%r11)\n    fldcw 0x28(%r11)\n"
"2:  movq 0x08(%r11), %rdi\n    movq 0x10(%r11), %rsi\n    movq 0x00(%r11), %r11\n"
"    xorl %eax, %eax\n    xorl %ebx, %ebx\n    xorl %ecx, %ecx\n    xorl %edx, %edx\n"
"    xorq %r8, %r8\n    xorq %r9, %r9\n    xorq %r10, %r10\n    xorq %r12, %r12\n"
"    xorq %r13, %r13\n    xorq %r14, %r14\n    xorq %r15, %r15\n    vzeroall\n"
"    call *%r11\n    movl $1, %edi\n    call bb_fiber_force_quit\n    ud2\n");

/* The thread's running fiber context (the PS4 keeps it in the TCB). */
static __thread FiberContext *thread_fiber;
__attribute__((used,noreturn)) void bb_fiber_force_quit(uint64_t value) {
    thread_fiber->return_val=value;
    bb_fiber_longjmp(thread_fiber);
}
static int valid(const Fiber *f) { return f->magic_start==SIGNATURE0 && f->magic_end==SIGNATURE1; }
static int32_t attach_context(Fiber *f,void *addr,uint64_t size) {
    if (size && size<MIN_CONTEXT) return FIBER_RANGE;
    if ((size&15) || !addr || !size || f->addr_context) return FIBER_INVALID;
    f->addr_context=addr; f->size_context=size;
    f->context_start=addr; f->context_end=(unsigned char *)addr+size;
    *(uint64_t *)addr=STACK_SIGNATURE;
    return 0;
}
static void start_entry(Fiber *f,uint64_t arg,void *stack,uint32_t *prev_state) {
    FiberData data={.entry=f->entry,.arg_on_initialize=f->arg_on_initialize,.arg_on_run_to=arg,
                    .stack_addr=stack,.state=prev_state,.fpucw=0x037f,.mxcsr=0x9fc0};
    bb_fiber_switch_entry(&data,(f->flags&SET_FPU)!=0);
}
static __attribute__((noreturn)) void switch_to(Fiber *f,uint64_t arg,FiberContext *ctx) {
    if (f->context) { ctx->arg_on_run_to=arg; bb_fiber_longjmp(f->context); }
    uint32_t *prev=NULL;
    if (ctx->prev_fiber) { prev=&ctx->prev_fiber->state; ctx->prev_fiber=NULL; }
    start_entry(f,arg,(unsigned char *)f->addr_context+f->size_context,prev);
    __builtin_trap();
}
static __attribute__((noreturn)) void switch_fiber(Fiber *cur,Fiber *f,uint64_t arg,FiberContext *ctx) {
    ctx->prev_fiber=cur; ctx->current_fiber=f;
    if (!f->addr_context) {
        ctx->prev_fiber=NULL;
        start_entry(f,arg,(void *)(ctx->rsp&~UINT64_C(15)),&cur->state);
    }
    switch_to(f,arg,ctx);
}
static ABI int32_t fiber_init_impl(Fiber *f,const char *name,FiberEntry entry,uint64_t arg,void *addr,uint64_t size,
                                   const uint32_t *opt,uint32_t flags,uint32_t build) {
    if (!f || !name || !entry) return FIBER_NULL;
    if (((uintptr_t)f&7) || ((uintptr_t)addr&15) || (opt && ((uintptr_t)opt&7))) return FIBER_ALIGNMENT;
    if (size && size<MIN_CONTEXT) return FIBER_RANGE;
    if ((size&15) || (!addr && size) || (addr && !size)) return FIBER_INVALID;
    if (opt && *opt!=OPT_SIGNATURE) return FIBER_INVALID;
    if (build>=0x03500000) flags|=SET_FPU;
    memset(f->name,0,sizeof(f->name)); strncpy(f->name,name,31);
    f->entry=entry; f->arg_on_initialize=arg; f->addr_context=addr; f->size_context=size;
    f->context=NULL; f->flags=flags;
    if (addr) {
        f->context_start=addr; f->context_end=(unsigned char *)addr+size;
        *(uint64_t *)addr=STACK_SIGNATURE;
    }
    f->magic_start=SIGNATURE0; f->magic_end=SIGNATURE1;
    __atomic_store_n(&f->state,IDLE,__ATOMIC_SEQ_CST);
    return 0;
}
static ABI int32_t fiber_init(Fiber *f,const char *name,FiberEntry entry,uint64_t arg,void *addr,uint64_t size,
                              const uint32_t *opt,uint32_t build) {
    return fiber_init_impl(f,name,entry,arg,addr,size,opt,0,build);
}
static ABI int32_t fiber_opt_init(uint32_t *opt) {
    if (!opt) return FIBER_NULL;
    if ((uintptr_t)opt&7) return FIBER_ALIGNMENT;
    *opt=OPT_SIGNATURE; return 0;
}
static ABI int32_t fiber_finalize(Fiber *f) {
    if (!f) return FIBER_NULL;
    if ((uintptr_t)f&7) return FIBER_ALIGNMENT;
    if (!valid(f)) return FIBER_INVALID;
    uint32_t expected=IDLE;
    return __atomic_compare_exchange_n(&f->state,&expected,TERMINATED,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST) ? 0 : FIBER_STATE;
}
static ABI int32_t fiber_run_impl(Fiber *f,void *addr,uint64_t size,uint64_t arg,uint64_t *on_return) {
    if (!f) return FIBER_NULL;
    if (((uintptr_t)f&7) || ((uintptr_t)addr&15)) return FIBER_ALIGNMENT;
    if (!valid(f)) return FIBER_INVALID;
    if (thread_fiber) return FIBER_PERMISSION;
    if (addr || size) { int32_t r=attach_context(f,addr,size); if (r<0) return r; }
    uint32_t expected=IDLE;
    if (!__atomic_compare_exchange_n(&f->state,&expected,RUN,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) return FIBER_STATE;
    FiberContext ctx;
    memset(&ctx,0,sizeof(ctx));
    ctx.current_fiber=f;
    thread_fiber=&ctx;
    if (!bb_fiber_setjmp(&ctx)) {
        if (f->addr_context) switch_to(f,arg,&ctx);
        start_entry(f,arg,(void *)(ctx.rsp&~UINT64_C(15)),NULL);
    }
    /* Back from sceFiberReturnToThread (or an entry that returned). */
    FiberContext *c=thread_fiber;
    Fiber *cur=c->current_fiber;
    c->current_fiber=NULL;
    __atomic_store_n(&cur->state,IDLE,__ATOMIC_SEQ_CST);
    if (c->return_val) { fprintf(stderr,"STOP: fiber '%s' entry returned\n",cur->name); exit(21); }
    if (on_return) *on_return=c->arg_on_return;
    thread_fiber=NULL;
    return 0;
}
static ABI int32_t fiber_switch_impl(Fiber *f,void *addr,uint64_t size,uint64_t arg,uint64_t *on_run) {
    if (!f) return FIBER_NULL;
    if (((uintptr_t)f&7) || ((uintptr_t)addr&15)) return FIBER_ALIGNMENT;
    if (!valid(f)) return FIBER_INVALID;
    FiberContext *g=thread_fiber;
    if (!g) return FIBER_PERMISSION;
    if (addr || size) { int32_t r=attach_context(f,addr,size); if (r<0) return r; }
    uint32_t expected=IDLE;
    if (!__atomic_compare_exchange_n(&f->state,&expected,RUN,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) return FIBER_STATE;
    Fiber *cur=g->current_fiber;
    if (!cur->addr_context) switch_fiber(cur,f,arg,g);
    FiberContext ctx;
    memset(&ctx,0,sizeof(ctx));
    if (!bb_fiber_setjmp(&ctx)) {
        cur->context=&ctx;
        switch_fiber(cur,f,arg,g);
    }
    g=thread_fiber;
    if (g->prev_fiber) { __atomic_store_n(&g->prev_fiber->state,IDLE,__ATOMIC_SEQ_CST); g->prev_fiber=NULL; }
    if (on_run) *on_run=g->arg_on_run_to;
    return 0;
}
static ABI int32_t fiber_run(Fiber *f,uint64_t arg,uint64_t *on_return) { return fiber_run_impl(f,NULL,0,arg,on_return); }
static ABI int32_t fiber_switch(Fiber *f,uint64_t arg,uint64_t *on_run) { return fiber_switch_impl(f,NULL,0,arg,on_run); }
static ABI int32_t fiber_self(Fiber **out) {
    if (!out) return FIBER_NULL;
    if (!thread_fiber) return FIBER_PERMISSION;
    *out=thread_fiber->current_fiber; return 0;
}
static ABI int32_t fiber_return(uint64_t on_return,uint64_t *on_run) {
    FiberContext *g=thread_fiber;
    if (!g) return FIBER_PERMISSION;
    Fiber *cur=g->current_fiber;
    if (cur->addr_context) {
        FiberContext ctx;
        memset(&ctx,0,sizeof(ctx));
        if (bb_fiber_setjmp(&ctx)) {
            g=thread_fiber;
            if (g->prev_fiber) { __atomic_store_n(&g->prev_fiber->state,IDLE,__ATOMIC_SEQ_CST); g->prev_fiber=NULL; }
            if (on_run) *on_run=g->arg_on_run_to;
            return 0;
        }
        cur->context=&ctx;
    }
    g->arg_on_return=on_return;
    bb_fiber_longjmp(g);
}
static ABI int32_t fiber_rename(Fiber *f,const char *name) {
    if (!f || !name) return FIBER_NULL;
    if (!valid(f)) return FIBER_INVALID;
    memset(f->name,0,sizeof(f->name)); strncpy(f->name,name,31); return 0;
}
static ABI int32_t fiber_frame_pointer(uint64_t *out) {
    if (!out) return FIBER_NULL;
    if (!thread_fiber) return FIBER_PERMISSION;
    *out=thread_fiber->rbp; return 0;
}
static const RuntimeExport exports[]={
    {"sceFiberInitialize",fiber_init}, {"_sceFiberInitializeWithInternalOptionImpl",fiber_init_impl},
    {"sceFiberOptParamInitialize",fiber_opt_init}, {"sceFiberFinalize",fiber_finalize},
    {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch}, {"sceFiberGetSelf",fiber_self},
    {"sceFiberReturnToThread",fiber_return}, {"_sceFiberAttachContextAndRun",fiber_run_impl},
    {"_sceFiberAttachContextAndSwitch",fiber_switch_impl}, {"sceFiberRename",fiber_rename},
    {"sceFiberGetThreadFramePointerAddress",fiber_frame_pointer},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
uintptr_t runtime_fiber_resolve(const char *name) { (void)name; return 0; }
#endif
