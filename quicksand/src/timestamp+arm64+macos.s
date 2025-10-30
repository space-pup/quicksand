# time+arm64+macos.s - macOS ARM64 assembly
# Assemble: as -o quicksand_now.o timestamp+arm64+macos.s
# Link: gcc -dynamiclib -o libquicksand_now.dylib quicksand_now.o

	.section __TEXT,__text,regular,pure_instructions
	.globl _quicksand_now
	.p2align 2

_quicksand_now:
	mrs     x0, CNTVCT_EL0
	ret
