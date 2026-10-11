@ A 2-byte .ARM.extab input section, linked last: makes the output .ARM.extab
@ end on a 2-byte boundary, as happens naturally in larger programs.
@ repro.cpp references extab_pad so --gc-sections keeps it.
	.section .ARM.extab.pad,"a",%progbits
	.balign 2
	.global extab_pad
extab_pad:
	.short 0
