#;# Assemble → object file
#; as   -o quicksand_now.o quicksand_now.s          # or  gcc -c -fPIC
#; gcc -shared -o libquicksand_now.so quicksand_now.o

	.section .text
	.globl  quicksand_now  #; make it visible to other modules
	.type   quicksand_now,@function
	.p2align 2

quicksand_now:
	MRS     x0, CNTVCT_EL0     #; Read virtual counter
	RET

	.size   quicksand_now, .-quicksand_now
