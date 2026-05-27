/*
 * arch.h - Architecture-agnostic register interface
 *
 * Each architecture provides:
 *   - corex_gp_regs_t   (general purpose register storage)
 *   - corex_fp_regs_t   (floating point register storage)
 *   - arch_read_gp_regs()
 *   - arch_read_fp_regs()
 *   - arch_fill_prstatus()
 *   - arch_get_elf_machine()
 */
#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <elf.h>
#include <sys/procfs.h>
#include <sys/ptrace.h>

#if defined(__x86_64__)

#include <sys/user.h>
typedef struct user_regs_struct  corex_gp_regs_t;
typedef struct user_fpregs_struct corex_fp_regs_t;

#elif defined(__aarch64__)

#include <sys/user.h>
#include <asm/ptrace.h>
typedef struct user_pt_regs     corex_gp_regs_t;
typedef struct user_fpsimd_state corex_fp_regs_t;

#else
#error "Unsupported architecture"
#endif

/* Read general-purpose registers for a stopped thread via ptrace.
 * Returns 0 on success. */
int arch_read_gp_regs(pid_t tid, corex_gp_regs_t *regs);

/* Read floating-point registers for a stopped thread via ptrace.
 * Returns 0 on success. */
int arch_read_fp_regs(pid_t tid, corex_fp_regs_t *regs);

/* Fill an elf_prstatus structure from our captured GP register state. */
void arch_fill_prstatus(struct elf_prstatus *prs, pid_t pid, pid_t tid,
                        int signo, const corex_gp_regs_t *gp_regs);

/* Return the ELF e_machine value for the current architecture. */
uint16_t arch_get_elf_machine(void);

/* Return the size of the FP register note data. */
size_t arch_fp_regset_size(void);

#endif /* ARCH_H */
