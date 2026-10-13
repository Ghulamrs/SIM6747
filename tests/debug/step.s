	.sect	".const"
L$str$0:
	.byte	120, 61, 37, 100, 10, 0
L$str$1:
	.byte	121, 61, 37, 100, 10, 0
	.text
_ZL5twicei:
	.dwpsn	file "step.cpp",line 3,column 12,is_stmt,isa 0
	STW	A15, *B15
	MV	B15, A15
	MVKL	56, B0
	MVKH	56, B0
	SUB	B15, B0, B15
	STW	A4, *-A15(44)
	.dwpsn	file "step.cpp",line 4,column 1,is_stmt,isa 0
	.dwpsn	file "step.cpp",line 5,column 5,is_stmt,isa 0
	LDW	*-A15(44), A4
	NOP	4
	SHL	A4, 1, A4
	STW	A4, *-A15(48)
	.dwpsn	file "step.cpp",line 6,column 5,is_stmt,isa 0
	LDW	*-A15(48), A4
	NOP	4
	B	L$return$_ZL5twicei
	NOP	5
	ZERO	A4
L$return$_ZL5twicei:
	MV	A15, B15
	LDW	*A15, A15
	NOP	4
	B	B3
	NOP	5
	.text
	.global _Z3addii
_Z3addii:
	.dwpsn	file "step.cpp",line 9,column 5,is_stmt,isa 0
	STW	A15, *B15
	MV	B15, A15
	STW	B3, *-A15(4)
	MVKL	56, B0
	MVKH	56, B0
	SUB	B15, B0, B15
	STW	A4, *-A15(44)
	MV	B4, A4
	STW	A4, *-A15(48)
	.dwpsn	file "step.cpp",line 10,column 1,is_stmt,isa 0
	.dwpsn	file "step.cpp",line 11,column 5,is_stmt,isa 0
	LDW	*-A15(44), A4
	NOP	4
	SUB	B15, 8, B15
	STW	A4, *B15
	LDW	*-A15(48), A4
	NOP	4
	MV	A4, A6
	LDW	*B15, A4
	NOP	4
	ADD	B15, 8, B15
	ADD	A4, A6, A4
	STW	A4, *-A15(52)
	.dwpsn	file "step.cpp",line 12,column 5,is_stmt,isa 0
	SUB	B15, 8, B15
	LDW	*-A15(52), A4
	NOP	4
	MVKL	L$_Z3addii$ret0, B3
	MVKH	L$_Z3addii$ret0, B3
	B	_ZL5twicei
	NOP	5
L$_Z3addii$ret0:
	ADD	B15, 8, B15
	STW	A4, *-A15(52)
	.dwpsn	file "step.cpp",line 13,column 5,is_stmt,isa 0
	LDW	*-A15(52), A4
	NOP	4
	B	L$return$_Z3addii
	NOP	5
	ZERO	A4
L$return$_Z3addii:
	LDW	*-A15(4), B3
	MV	A15, B15
	LDW	*A15, A15
	NOP	4
	B	B3
	NOP	5
	.sect	".c6xabi.exidx:.text"
	.align	4
	.ulong	$EXIDX_FUNC(_Z3addii)
	.ulong	0x83ff0207
	.text
	.text
	.global main
main:
	.dwpsn	file "step.cpp",line 16,column 5,is_stmt,isa 0
	STW	A15, *B15
	MV	B15, A15
	STW	B3, *-A15(4)
	MVKL	56, B0
	MVKH	56, B0
	SUB	B15, B0, B15
	.dwpsn	file "step.cpp",line 17,column 1,is_stmt,isa 0
	.dwpsn	file "step.cpp",line 18,column 5,is_stmt,isa 0
	SUB	B15, 8, B15
	MVKL	1, A4
	MVKH	1, A4
	SUB	B15, 8, B15
	STW	A4, *B15
	MVKL	2, A4
	MVKH	2, A4
	MV	A4, B4
	LDW	*B15, A4
	NOP	4
	ADD	B15, 8, B15
	MVKL	L$main$ret0, B3
	MVKH	L$main$ret0, B3
	B	_Z3addii
	NOP	5
L$main$ret0:
	ADD	B15, 8, B15
	STW	A4, *-A15(44)
	.dwpsn	file "step.cpp",line 19,column 5,is_stmt,isa 0
	SUB	B15, 16, B15
	MVKL	L$str$0, A4
	MVKH	L$str$0, A4
	ADD	B15, 4, B0
	STW	A4, *B0
	LDW	*-A15(44), A4
	NOP	4
	ADD	B15, 8, B0
	STW	A4, *B0
	MVKL	L$main$ret1, B3
	MVKH	L$main$ret1, B3
	B	printf
	NOP	5
L$main$ret1:
	ADD	B15, 16, B15
	.dwpsn	file "step.cpp",line 20,column 5,is_stmt,isa 0
	SUB	B15, 8, B15
	LDW	*-A15(44), A4
	NOP	4
	SUB	B15, 8, B15
	STW	A4, *B15
	MVKL	4, A4
	MVKH	4, A4
	MV	A4, B4
	LDW	*B15, A4
	NOP	4
	ADD	B15, 8, B15
	MVKL	L$main$ret2, B3
	MVKH	L$main$ret2, B3
	B	_Z3addii
	NOP	5
L$main$ret2:
	ADD	B15, 8, B15
	STW	A4, *-A15(48)
	.dwpsn	file "step.cpp",line 21,column 5,is_stmt,isa 0
	SUB	B15, 16, B15
	MVKL	L$str$1, A4
	MVKH	L$str$1, A4
	ADD	B15, 4, B0
	STW	A4, *B0
	LDW	*-A15(48), A4
	NOP	4
	ADD	B15, 8, B0
	STW	A4, *B0
	MVKL	L$main$ret3, B3
	MVKH	L$main$ret3, B3
	B	printf
	NOP	5
L$main$ret3:
	ADD	B15, 16, B15
	.dwpsn	file "step.cpp",line 22,column 5,is_stmt,isa 0
	LDW	*-A15(48), A4
	NOP	4
	SUB	B15, 8, B15
	STW	A4, *B15
	MVKL	14, A4
	MVKH	14, A4
	MV	A4, A6
	LDW	*B15, A4
	NOP	4
	ADD	B15, 8, B15
	SUB	A4, A6, A4
	B	L$return$main
	NOP	5
	ZERO	A4
L$return$main:
	LDW	*-A15(4), B3
	MV	A15, B15
	LDW	*A15, A15
	NOP	4
	B	B3
	NOP	5
	.sect	".c6xabi.exidx:.text"
	.align	4
	.ulong	$EXIDX_FUNC(main)
	.ulong	0x83ff0207
	.text
	.global	__c6xabi_unwind_cpp_pr3
	.symdepend	"__c6xabi_unwind_cpp_pr3", ".c6xabi.exidx:.text"
	.ref	printf
