# Hand-written so the absolute relocation forms actually appear: a C compiler
# targeting -fno-pie still prefers RIP-relative, so R_X86_64_32S and
# R_X86_64_32 need asking for by name.
	.text
	.globl	asm_probe
	.type	asm_probe, @function
asm_probe:
	movq	$asm_val, %rax		# R_X86_64_32S: sign-extended absolute
	movl	(%rax), %eax
	movl	$asm_val, %edx		# R_X86_64_32:  zero-extended absolute
	addl	(%rdx), %eax
	movq	asm_ptr(%rip), %rcx	# R_X86_64_PC32
	addl	(%rcx), %eax
	ret
	.size	asm_probe, .-asm_probe

	.data
	.align	8
	.globl	asm_ptr
asm_ptr:
	.quad	asm_val			# R_X86_64_64
	.globl	asm_val
	.align	4
asm_val:
	.long	3

	.section	.note.GNU-stack,"",@progbits
