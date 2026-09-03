#include <ppc-asm.h>

	.globl texCoordTrans
texCoordTrans:
	psq_l        f0,0(r4),0,0
	psq_l        f3,0(r5),0,0
	psq_l        f2,32(r4),0,0
	ps_muls0     f6,f0,f3			// FREE: f0	
	psq_l        f1,16(r4),0,0
	psq_l        f4,8(r5),1,0
	ps_madds1    f7,f1,f3,f6		// FREE: f6/f1/f3
	lis          r9,Unit16@ha
	lfs          f9,Unit16@l(r9)	
	ps_madds0    f8,f2,f4,f7		// FREE: f2/f4/f7
	psq_l        f5,0(r6),0,0
	ps_madd      f0,f9,f5,f8		// FREE: f5/f8
	ps_div       f3,f0,f9			// Used: f3
	psq_st       f3,0(r3),0,0
	blr

	.globl ps_MatrixMultVec4x4
	//r3 = mtx, r4= vec
ps_MatrixMultVec4x4:
		// f0(0) = mtx0  , f0(1) = mtx1
	psq_l        f0,0(r3),0,0
		// f1(0) = mtx2  , f1(1) = mtx3
	psq_l        f1,8(r3),0,0
		// f2(0) = mtx4  , f2(1) = mtx5
	psq_l        f2,16(r3),0,0
		// f3(0) = mtx6  , f3(1) = mtx7
	psq_l        f3,24(r3),0,0
		// f4(0) = mtx8  , f4(1) = mtx9
	psq_l        f4,32(r3),0,0
		// f5(0) = mtx10 , f5(1) = mtx11
	psq_l        f5,40(r3),0,0
		// f6(0) = mtx12 , f6(1) = mtx13
	psq_l        f6,48(r3),0,0
		// f7(0) = mtx14 , f7(1) = mtx15
	psq_l        f7,56(r3),0,0
		// f8(0) = x     , f8(1) = x
	lfs          f8,0(r4)
		// f9(0) = y     , f9(1) = y
	lfs          f9,4(r4)		
		// f10(0) = z    , f10(1) = z
	lfs          f10,8(r4)
		// f11(0) = w    , f11(1) = w
	lfs          f11,12(r4)	
	//---------------------Loaded--------------------
		//f12(0) = f0(0) * f8(0) , f12(1) = f0(1) * f8(1)
		//f12(0) = mtx0  * x     , f12(1) = mtx1  * x
	ps_mul       f12,f0,f8									//free: f0
		//f13(0) = f1(0) * f3(0) , f13(1) = f1(1) * f3(1)
		//f13(0) = mtx2  * x     , f13(1) = mtx3  * x
	ps_mul       f13,f1,f8									//free: f1/f8
		// + y
	ps_madd      f14,f2,f9,f12								//free: f2/f12
	ps_madd      f15,f3,f9,f13								//free: f3/f9/f13
		// + z
	ps_madd      f16,f4,f10,f14								//free: f4/f14
	ps_madd      f17,f5,f10,f15								//free: f5/f10/f15
		// + w
	ps_madd      f18,f6,f11,f16								//free: f6/f16
	ps_madd      f19,f7,f11,f17								//free: f7/f11/f17

	psq_st       f18,0(r4),0,0
	psq_st       f19,8(r4),0,0
	blr

	.globl ps_MatrixMultVec3x3
	//r3 = mtx, r4= vec
ps_MatrixMultVec3x3:
		// f0(0) = mtx0  , f0(1) = mtx1
	psq_l        f0,0(r3),0,0
		// f1(0) = mtx2  , f1(1) = 1
	psq_l        f1,8(r3),1,0
		// f2(0) = mtx4  , f2(1) = mtx5
	psq_l        f2,16(r3),0,0
		// f3(0) = mtx6  , f3(1) = 1
	psq_l        f3,24(r3),1,0
		// f4(0) = mtx8  , f4(1) = mtx9
	psq_l        f4,32(r3),0,0
		// f5(0) = mtx10 , f5(1) = 1
	psq_l        f5,40(r3),1,0
		// f8(0) = x     , f8(1) = y
	psq_l        f8,0(r4),0,0
		// f9(0) = z     , f9(1) = 1
	psq_l        f9,8(r4),1,0	
	//---------------------Loaded--------------------
		//f12(0) = f0(0) * f8(0) , f12(1) = f0(1) * f8(1)
		//f12(0) = mtx0  * x     , f12(1) = mtx1  * x
	ps_muls0     f12,f0,f8									//free: f0
		//f13(0) = f1(0) * f8(0) , f13(1) = f1(1) * f8(1)
		//f13(0) = mtx2  * x     , f13(1) = x
	ps_muls0     f13,f1,f8									//free: f1
		// + y
	ps_madds1    f14,f2,f8,f12								//free: f2/f12
	ps_madds1    f15,f3,f8,f13								//free: f3/f8/f13
		// + z
	ps_madds0    f16,f4,f9,f14								//free: f4/f14
	ps_madds0    f17,f5,f9,f15								//free: f5/f9/f15

	psq_st       f16,0(r4),0,0
	stfs         f17,8(r4)
	blr

	.globl ps_MatrixCopy
	//r3 = mtx, r4= src
ps_MatrixCopy:
	psq_l        f0,0(r4),0,0
	psq_l        f1,8(r4),0,0
	psq_st       f0,0(r3),0,0
	psq_l        f2,16(r4),0,0
	psq_st       f1,8(r3),0,0
	psq_l        f3,24(r4),0,0
	psq_st       f2,16(r3),0,0
	psq_l        f4,32(r4),0,0
	psq_st       f3,24(r3),0,0
	psq_l        f5,40(r4),0,0
	psq_st       f4,32(r3),0,0
	psq_l        f6,48(r4),0,0
	psq_st       f5,40(r3),0,0
	psq_l        f7,56(r4),0,0
	psq_st       f6,48(r3),0,0
	psq_st       f7,56(r3),0,0
	blr

	.globl ps_MatrixTranslate
	//r3 = mtx, r4= vec
ps_MatrixTranslate:
		// f0(0) = mtx0  , f0(1) = mtx1
	psq_l        f0,0(r3),0,0
		// f1(0) = mtx2  , f1(1) = mtx3
	psq_l        f1,8(r3),0,0
		// f2(0) = mtx4  , f2(1) = mtx5
	psq_l        f2,16(r3),0,0
		// f3(0) = mtx6  , f3(1) = mtx7
	psq_l        f3,24(r3),0,0
		// f4(0) = mtx8  , f4(1) = mtx9
	psq_l        f4,32(r3),0,0
		// f5(0) = mtx10 , f5(1) = mtx11
	psq_l        f5,40(r3),0,0
		// f6(0) = mtx12 , f6(1) = mtx13
	psq_l        f6,48(r3),0,0
		// f7(0) = mtx14 , f7(1) = mtx15
	psq_l        f7,56(r3),0,0
		// f8(0) = x     , f8(1) = y
	psq_l        f8,0(r4),0,0
		// f9(0) = z     , f9(1) = 1
	psq_l        f9,8(r4),1,0	
	//---------------------Loaded--------------------
		//f12(0) = f0(0) * f8(0) , f12(1) = f0(1) * f8(1)
		//f12(0) = mtx0  * x     , f12(1) = mtx1  * x
	ps_muls0     f12,f0,f8								//free: f0
		//f13(0) = f1(0) * f3(0) , f13(1) = f1(1) * f3(1)
		//f13(0) = mtx2  * x     , f13(1) = mtx3  * x
	ps_muls0     f13,f1,f8								//free: f1
		// + y
	ps_madds1    f14,f2,f8,f12							//free: f2/f12
	ps_madds1    f15,f3,f8,f13							//free: f3/f9/f13
		// + z
	ps_madds0    f16,f4,f9,f14							//free: f4/f14
	ps_madds0    f17,f5,f9,f15							//free: f5/f9/f15
		// mtx +=
	ps_add       f18,f6,f16								//free: f6/f16
	ps_add       f19,f7,f17								//free: f7/f17

	psq_st       f18,48(r3),0,0
	psq_st       f19,56(r3),0,0
	blr

	.globl ps_MatrixScale
	//r3 = mtx, r4= vec
ps_MatrixScale:
		// f0(0) = mtx0  , f0(1) = mtx1
	psq_l        f0,0(r3),0,0
		// f1(0) = mtx2  , f1(1) = mtx3
	psq_l        f1,8(r3),0,0
		// f2(0) = mtx4  , f2(1) = mtx5
	psq_l        f2,16(r3),0,0
		// f3(0) = mtx6  , f3(1) = mtx7
	psq_l        f3,24(r3),0,0
		// f4(0) = mtx8  , f4(1) = mtx9
	psq_l        f4,32(r3),0,0
		// f5(0) = mtx10 , f5(1) = mtx11
	psq_l        f5,40(r3),0,0
		// f8(0) = x     , f8(1) = y
	psq_l        f8,0(r4),0,0
		// f9(0) = z     , f9(1) = 1
	psq_l        f9,8(r4),1,0		
	//---------------------Loaded--------------------
		//f12(0) = f0(0) * f8(0) , f12(1) = f0(1) * f8(1)
		//f12(0) = mtx0  * x     , f12(1) = mtx1  * x
	ps_muls0     f12,f0,f8							//free: f0
		//f13(0) = f1(0) * f3(0) , f13(1) = f1(1) * f3(1)
		//f13(0) = mtx2  * x     , f13(1) = mtx3  * x
	ps_muls0     f13,f1,f8							//free: f1
		// + y
	ps_muls1     f14,f2,f8							//free: f2/f12
	ps_muls1     f15,f3,f8							//free: f3/f8/f13
		// + z
	ps_muls0     f16,f4,f9							//free: f4/f14
	ps_muls0     f17,f5,f9							//free: f5/f9/f15

	psq_st       f12,0(r3),0,0
	psq_st       f13,8(r3),0,0
	psq_st       f14,16(r3),0,0
	psq_st       f15,24(r3),0,0
	psq_st       f16,32(r3),0,0
	psq_st       f17,40(r3),0,0
	blr

	.globl ps_MatrixMultiply
	//r3 = mtx, r4= rtx
ps_MatrixMultiply:
		// f0(0) = mtx0  , f0(1) = mtx1
	psq_l        f0,0(r3),0,0
		// f1(0) = mtx2  , f1(1) = mtx3
	psq_l        f1,8(r3),0,0
		// f2(0) = mtx4  , f2(1) = mtx5
	psq_l        f2,16(r3),0,0
		// f3(0) = mtx6  , f3(1) = mtx7
	psq_l        f3,24(r3),0,0
		// f4(0) = mtx8  , f4(1) = mtx9
	psq_l        f4,32(r3),0,0
		// f5(0) = mtx10 , f5(1) = mtx11
	psq_l        f5,40(r3),0,0
		// f6(0) = mtx12 , f6(1) = mtx13
	psq_l        f6,48(r3),0,0
		// f7(0) = mtx14 , f7(1) = mtx15
	psq_l        f7,56(r3),0,0
	//////////////////////////////////////////////////////
		// f8(0) = rtx0  , f8(1) = rtx1
	psq_l        f8,0(r4),0,0
		//matrix[0-3]*rightMatrix[0]
	ps_muls0     f9,f0,f8
	ps_muls0     f10,f1,f8					
		//+ matrix[4-7]*rightMatrix[1]	
	ps_madds1    f11,f2,f8,f9		// Free: f9
	ps_madds1    f12,f3,f8,f10		// Free: f8,f10
		// f13(0) = rtx2  , f13(1) = rtx3
	psq_l        f13,8(r4),0,0
		//+ matrix[8-11]*rightMatrix[2]
	ps_madds0    f14,f4,f13,f11		// Free: f11
	ps_madds0    f15,f5,f13,f12		// Free: f12		
		//+ matrix[12-15]*rightMatrix[3]	
	ps_madds1    f8,f6,f13,f14		// Used: f8  Free: f14
	ps_madds1    f9,f7,f13,f15		// Used: f9  Free: f15,f13
	//STORE IT
	psq_st       f8,0(r3),0,0		//           Free: f8
	psq_st       f9,8(r3),0,0		//           Free: f9
	//////////////////////////////////////////////////////
		// f10(0) = rtx4  , f10(1) = rtx5
	psq_l        f10,16(r4),0,0
		//matrix[0-3]*rightMatrix[4]
	ps_muls0     f11,f0,f10
	ps_muls0     f12,f1,f10					
		//+ matrix[4-7]*rightMatrix[5]	
	ps_madds1    f8,f2,f10,f11		// Free: f11
	ps_madds1    f9,f3,f10,f12		// Free: f10,f12
		// f13(0) = rtx6  , f13(1) = rtx7
	psq_l        f13,24(r4),0,0
		//+ matrix[8-11]*rightMatrix[6]
	ps_madds0    f14,f4,f13,f8		// Free: f8
	ps_madds0    f15,f5,f13,f9		// Free: f9		
		//+ matrix[12-15]*rightMatrix[7]	
	ps_madds1    f10,f6,f13,f14		// Used: f10 Free: f14
	ps_madds1    f11,f7,f13,f15		// Used: f11 Free: f15,f13
	//STORE IT
	psq_st       f10,16(r3),0,0		//           Free: f10
	psq_st       f11,24(r3),0,0		//           Free: f11
	//////////////////////////////////////////////////////
		// f8(0) = rtx8  , f8(1) = rtx9
	psq_l        f8,32(r4),0,0
		//matrix[0-3]*rightMatrix[8]
	ps_muls0     f11,f0,f8
	ps_muls0     f12,f1,f8					
		//+ matrix[4-7]*rightMatrix[9]	
	ps_madds1    f13,f2,f8,f11		//           Free: f11
	ps_madds1    f14,f3,f8,f12		//           Free: f8,f12
		// f13(0) = rtx10  , f13(1) = rtx11
	psq_l        f11,40(r4),0,0		// Used: f11
		//+ matrix[8-11]*rightMatrix[10]
	ps_madds0    f9,f4,f11,f13		//           Free: f13
	ps_madds0    f10,f5,f11,f14		//           Free: f14		
		//+ matrix[12-15]*rightMatrix[11]	
	ps_madds1    f8,f6,f11,f9		// Used: f8  Free: f9
	ps_madds1    f12,f7,f11,f10		// Used: f12 Free: f10,f11
	//STORE IT
	psq_st       f8,32(r3),0,0		//           Free: f8
	psq_st       f12,40(r3),0,0		//           Free: f12
	
	//////////////////////////////////////////////////////
		// f9(0) = rtx12  , f9(1) = rtx13
	psq_l        f9,48(r4),0,0
		//matrix[0-3]*rightMatrix[12]
	ps_muls0     f10,f0,f9			//           Free: f0
	ps_muls0     f11,f1,f9			//           Free: f1	
		//+ matrix[4-7]*rightMatrix[13]	
	ps_madds1    f8,f2,f9,f10		// Used: f8  Free: f10,f2
	ps_madds1    f12,f3,f9,f11		// Used: f12 Free: f9,f11,f3
		// f13(0) = rtx14  , f13(1) = rtx15
	psq_l        f11,56(r4),0,0		// Used: f11
		//+ matrix[8-11]*rightMatrix[14]
	ps_madds0    f0,f4,f11,f8		// Used: f0  Free: f8,f4
	ps_madds0    f1,f5,f11,f12		// Used: f1  Free: f12,f5	
		//+ matrix[12-15]*rightMatrix[15]	
	ps_madds1    f2,f6,f11,f0		// Used: f2  Free: f0,f6
	ps_madds1    f3,f7,f11,f1		// Used: f3  Free: f1,f11
	//STORE IT
	psq_st       f2,48(r3),0,0		//           Free: f2
	psq_st       f3,56(r3),0,0		//           Free: f3
	blr

	.globl ps_mtx_fix2float4x4
	//r3 = mtx, f1 = div
ps_mtx_fix2float4x4:
		// f0(0) = mtx0  , f0(1) = mtx1
	psq_l        f0,0(r3),0,0
		// f1(0) = div     , f1(1) = div
	//lfs          f1,0(r4)
		// f2(0) = mtx2  , f2(1) = mtx3
	psq_l        f2,8(r3),0,0
		// f3(0) = mtx4  , f3(1) = mtx5
	psq_l        f3,16(r3),0,0
		// f4(0) = mtx6  , f4(1) = mtx7
	psq_l        f4,24(r3),0,0
		// f5(0) = mtx8  , f5(1) = mtx9
	psq_l        f5,32(r3),0,0
		// f6(0) = mtx10 , f6(1) = mtx11
	psq_l        f6,40(r3),0,0
		// f7(0) = mtx12 , f7(1) = mtx13
	psq_l        f7,48(r3),0,0
		// f8(0) = mtx14 , f8(1) = mtx15
	psq_l        f8,56(r3),0,0

	ps_div       f9,f0,f1							//free: f0
	ps_div       f10,f2,f1							//free: f2
	ps_div       f0,f3,f1							//free: f3
	ps_div       f2,f4,f1							//free: f4
	ps_div       f3,f5,f1							//free: f5
	ps_div       f4,f6,f1							//free: f6
	ps_div       f5,f7,f1							//free: f7
	ps_div       f6,f8,f1							//free: f7
	psq_st       f9,0(r3),0,0
	psq_st       f10,8(r3),0,0
	psq_st       f0,16(r3),0,0
	psq_st       f2,24(r3),0,0
	psq_st       f3,32(r3),0,0
	psq_st       f4,40(r3),0,0
	psq_st       f5,48(r3),0,0
	psq_st       f6,56(r3),0,0
	blr

	.globl ps_mtx_fix2float3x4
	//r3 = mtx, f1 = div
ps_mtx_fix2float3x4:
		// f0(0) = mtx0  , f0(1) = mtx1
	psq_l        f0,0(r3),0,0
		// f1(0) = div     , f1(1) = div
	//lfs          f1,0(r4)
		// f2(0) = mtx2  , f2(1) = mtx3
	psq_l        f2,8(r3),0,0
		// f3(0) = mtx4  , f3(1) = mtx5
	psq_l        f3,16(r3),0,0
		// f4(0) = mtx6  , f4(1) = mtx7
	psq_l        f4,24(r3),0,0
		// f5(0) = mtx8  , f5(1) = mtx9
	psq_l        f5,32(r3),0,0
		// f6(0) = mtx10 , f6(1) = mtx11
	psq_l        f6,40(r3),0,0

	ps_div       f7,f0,f1							//free: f0
	ps_div       f9,f2,f1							//free: f2
	ps_div       f0,f3,f1							//free: f3
	ps_div       f2,f4,f1							//free: f4
	ps_div       f3,f5,f1							//free: f5
	ps_div       f4,f6,f1							//free: f6
	psq_st       f7,0(r3),0,0
	psq_st       f9,8(r3),0,0
	psq_st       f0,16(r3),0,0
	psq_st       f2,24(r3),0,0
	psq_st       f3,32(r3),0,0
	psq_st       f4,40(r3),0,0
	blr

	.globl setFinalColorCombine
setFinalColorCombine:		
		// 32
	lis          r9,Unit32@ha
	//r3 = cA, r4 = cB
		// f0(0) = alpha  , f0(1) = blue
	psq_l        f0,0(r3),0,1
		// f1(0) = green  , f1(1) = red
	psq_l        f1,2(r3),0,1
		// f4(0) = alpha  , f4(1) = alpha
	ps_merge00   f4,f0,f0
		// f2(0) = cB.a  , f2(1) = cB.b
	psq_l        f2,0(r4),0,2
		//f5(0) = a * a , f5(1) = b  * a
	ps_mul       f5,f0,f4					//Used: f5   Free: f0
		// f7(0) = 32    , f7(1) = 32
	lfs          f7,Unit32@l(r9)
		// f3(0) = cB.g  , f3(1) = cB.r
	psq_l        f3,2(r4),0,2		
		// 32 - alpha
	ps_sub       f8,f7,f4					//Used: f8   Free: f7
		//f6(0) = g * a , f6(1) = r  * a
	ps_mul       f6,f1,f4					//Used: f6   Free: f1
		//
	ps_madds0    f9,f2,f8,f5
	ps_madds0    f10,f3,f8,f6
	psq_st       f9,0(r3),0,3
	psq_st       f10,2(r3),0,3
	blr

	.section .data
	.balign 4
Unit16:
	.float	16.0
Unit32:
	.float	32.0
