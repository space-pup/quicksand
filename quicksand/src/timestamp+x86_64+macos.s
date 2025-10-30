# timestamp+x86_64+macos.s - macOS x86_64 assembly
# Assemble: as -o quicksand_now.o timestamp+x86_64+macos.s
# Link: gcc -dynamiclib -o libquicksand_now.dylib quicksand_now.o

	.section __TEXT,__text,regular,pure_instructions
	.globl _quicksand_now
	.p2align 4, 0x90

_quicksand_now:
	rdtsc
	shlq    $32, %rdx
	orq     %rdx, %rax
	ret
