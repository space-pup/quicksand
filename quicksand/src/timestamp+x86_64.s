# Assemble -> object file
# as   -o quicksand_now.o quicksand_now.s          # or  gcc -c -fPIC
# gcc -shared -o libquicksand_now.so quicksand_now.o

	.section .text
	.globl  quicksand_now
	.type   quicksand_now,@function
	.p2align 4

quicksand_now:
	rdtsc            # RDX:RAX <- TSC
	shlq $32, %rdx   # shift upper 32 bits into RDX
	orq  %rdx, %rax  # combine them -> 64‑bit value in RAX
	ret

	.size   quicksand_now, .-quicksand_now
