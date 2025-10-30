; clang -target x86_64-w64-mingw32 -shared -o quicksand_now.dll quicksand_now.s

	.intel_syntax noprefix            ; Intel syntax – common for Win
	.section .text
	.global   quicksand_now
	.type     quicksand_now,@function

quicksand_now:
	rdtsc
	shl     rdx, 32
	or      rax, rdx
	ret

	.size    quicksand_now, .-quicksand_now
