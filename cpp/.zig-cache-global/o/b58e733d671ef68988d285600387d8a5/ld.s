.text
.globl calloc
.type calloc, %function;
.symver calloc, calloc@@GLIBC_2.2.5
calloc:
.globl __libc_memalign
.type __libc_memalign, %function;
.symver __libc_memalign, __libc_memalign@@GLIBC_2.2.5
__libc_memalign:
.globl malloc
.type malloc, %function;
.symver malloc, malloc@@GLIBC_2.2.5
malloc:
.globl free
.type free, %function;
.symver free, free@@GLIBC_2.2.5
free:
.globl _dl_mcount
.type _dl_mcount, %function;
.symver _dl_mcount, _dl_mcount@@GLIBC_2.2.5
_dl_mcount:
.globl realloc
.type realloc, %function;
.symver realloc, realloc@@GLIBC_2.2.5
realloc:
.globl __tls_get_addr
.type __tls_get_addr, %function;
.symver __tls_get_addr, __tls_get_addr@@GLIBC_2.3
__tls_get_addr:
.data
.globl __libc_stack_end
.type __libc_stack_end, %object;
.size __libc_stack_end, 8;
.symver __libc_stack_end, __libc_stack_end@@GLIBC_2.2.5
__libc_stack_end:
.globl _r_debug
.type _r_debug, %object;
.size _r_debug, 40;
.symver _r_debug, _r_debug@@GLIBC_2.2.5
_r_debug:
.globl __rseq_offset
.type __rseq_offset, %object;
.size __rseq_offset, 8;
.symver __rseq_offset, __rseq_offset@@GLIBC_2.35
__rseq_offset:
.globl __rseq_flags
.type __rseq_flags, %object;
.size __rseq_flags, 4;
.symver __rseq_flags, __rseq_flags@@GLIBC_2.35
__rseq_flags:
.globl __rseq_size
.type __rseq_size, %object;
.size __rseq_size, 4;
.symver __rseq_size, __rseq_size@@GLIBC_2.35
__rseq_size:
