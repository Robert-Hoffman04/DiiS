/*
    Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#ifndef _matrixmath_h_
#define _matrixmath_h_

#include <ogcsys.h>
#include <gccore.h>


extern "C" {

// We must check if they are already defined in libogc.
// If they are, use the official functions. If not, use our own.

#ifndef guMtx44Identity
	void ps_guMtx44Identity(register Mtx44 mt);
	#define guMtx44Identity ps_guMtx44Identity
#endif

#ifndef guMtx44Copy
	void ps_guMtx44Copy(register Mtx44 src, register Mtx44 dst);
	#define guMtx44Copy ps_guMtx44Copy
#endif

#ifndef guMtx44Concat
	void ps_guMtx44Concat(const register Mtx44 a, const register Mtx44 b, register Mtx44 ab);
	#define guMtx44Concat ps_guMtx44Concat
#endif

#ifndef guMtx44Transpose
	void ps_guMtx44Transpose(register Mtx44 src, register Mtx44 xPose);
	#define guMtx44Transpose ps_guMtx44Transpose
#endif

#ifndef guMtx44Scale
	void ps_guMtx44Scale(register Mtx44 mt,register f32 xS,register f32 yS,register f32 zS);
	#define guMtx44Scale ps_guMtx44Scale
#endif

#ifndef guMtxMultQuat
	void ps_guMtxMultQuat(register Mtx m, register guQuaternion *a, register guQuaternion *d);
	#define guMtxMultQuat ps_guMtxMultQuat
#endif

#ifndef guMtx44MultQuat
	void ps_guMtx44MultQuat(register Mtx44 mt, register guQuaternion *src,register guQuaternion *dst);
	#define guMtx44MultQuat ps_guMtx44MultQuat
#endif

#ifndef guMtxInvXPose

	u32 ps_guMtxInvXpose_copyFromLibogc(register Mtx src, register Mtx xPose);
	//This should already be defined in libogc, actually.
	#define guMtxInvXPose ps_guMtxInvXpose_copyFromLibogc
#endif

}



void c_guMtx44MultVec ( const Mtx44 m, const guVector *src, guVector *dst ){
    guVector vTmp;
    f32 w;
    // a Vec has a 4th implicit 'w' coordinate of 1
    vTmp.x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z + m[0][3];
    vTmp.y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z + m[1][3];
    vTmp.z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z + m[2][3];
    w      = m[3][0]*src->x + m[3][1]*src->y + m[3][2]*src->z + m[3][3];
    w = 1.0f/w;

    // copy back
    dst->x = vTmp.x * w;
    dst->y = vTmp.y * w;
    dst->z = vTmp.z * w;
}


void c_guMtx44ApplyTrans(Mtx44 src, Mtx44 dst, const f32 xT, const f32 yT, const f32 zT){
	if ( src != dst ){
		dst[0][0] = src[0][0];    dst[0][1] = src[0][1];    dst[0][2] = src[0][2];
		dst[1][0] = src[1][0];    dst[1][1] = src[1][1];    dst[1][2] = src[1][2];
		dst[2][0] = src[2][0];    dst[2][1] = src[2][1];    dst[2][2] = src[2][2];
		dst[3][0] = src[3][0];    dst[3][1] = src[3][1];    dst[3][2] = src[3][2];
	}

	dst[0][3] = src[0][0]*xT + src[0][1]*yT + src[0][2]*zT + src[0][3];
	dst[1][3] = src[1][0]*xT + src[1][1]*yT + src[1][2]*zT + src[1][3];
	dst[2][3] = src[2][0]*xT + src[2][1]*yT + src[2][2]*zT + src[2][3];
	dst[3][3] = src[3][0]*xT + src[3][1]*yT + src[3][2]*zT + src[3][3];
}





// These are unfinished
//TODO

#ifndef guMtx44ApplyTrans
	/*
	// This one is actually slightly slower!!
	void ps_guMtx44ApplyTrans(register Mtx44 src, register Mtx44 dst, register f32 xT, register f32 yT, register f32 zT);
	// Darn it, intertwining the instructions doesn't speed it up!
	#define guMtx44ApplyTrans ps_guMtx44ApplyTrans	
	//*/
	#define guMtx44ApplyTrans c_guMtx44ApplyTrans
#endif

#ifndef guMtx44MultVec
	// ps_guMtx44MultVec is a copy of ps_guMtx44MultQuat
	/*
	void ps_guMtx44MultVec(register Mtx44 mt, register guVector *src, register guVector *dst);
	#define guMtx44MultVec ps_guMtx44MultVec
	//*/
	#define guMtx44MultVec c_guMtx44MultVec
#endif




//--DCN: My added fix-to-float function
// (To replace "vector_fix2float")
//////////////////////////////////////////////////
//////////////////////////////////////////////////
//////////////////////////////////////////////////

void fix2floatGX(Mtx44 matrix, const float d){
					  
	//--DCN: Make into paired-single!

	const float divisor = 1.0f / d;  // 1/4096 = 0.000244140625
	for(int i = 0; i < 4; i++){
		for(int j = 0; j < 4; j++){
			matrix[i][j] *= divisor;
		}
	}
}


void Mtx44Fix2float(Mtx44& mtx, const float divisor){

	register f32 m00_m01, m02_m03;
	register f32 m10_m11, m12_m13;
	register f32 m20_m21, m22_m23;
	register f32 m30_m31, m32_m33;

	register char *m = (char*)(mtx);

	register f32 shift = 1.0f / divisor;

	__asm__ __volatile__ (
		"psq_l		%0,  0(%8), 0, 0\n"		// load m00_m01
		"psq_l		%1,  8(%8), 0, 0\n"		// load m02_m03
		"ps_mul		%0, %0, %9\n"			// m00_m01 / shift
		"psq_l		%2, 16(%8), 0, 0\n"		// load m10_m11
		"psq_st		%0,  0(%8), 0, 0\n"		// store m00_m01
		"ps_mul		%1, %1, %9\n"			// m02_m03 / shift
		"psq_l		%3, 24(%8), 0, 0\n"		// load m12_m13
		"psq_st		%1,  8(%8), 0, 0\n"		// store m02_m03
		"ps_mul		%2, %2, %9\n"			// m10_m11 / shift
		"psq_l		%4, 32(%8), 0, 0\n"		// load m20_m21
		"psq_st		%2, 16(%8), 0, 0\n"		// store m10_m11
		"ps_mul		%3, %3, %9\n"			// m12_m13 / shift
		"psq_l		%5, 40(%8), 0, 0\n"		// load m22_m23
		"psq_st		%3, 24(%8), 0, 0\n"		// store m12_m13
		"ps_mul		%4, %4, %9\n"			// m20_m21 / shift
		"psq_l		%6, 48(%8), 0, 0\n"		// load m30_m31
		"psq_st		%4, 32(%8), 0, 0\n"		// store m20_m21
		"ps_mul		%5, %5, %9\n"			// m22_m23 / shift
		"psq_l		%7, 56(%8), 0, 0\n"		// load m32_m33
		"psq_st		%5, 40(%8), 0, 0\n"		// store m22_m23
		"ps_mul		%6, %6, %9\n"			// m30_m31 / shift
		"psq_st		%6, 48(%8), 0, 0\n"		// store m30_m31	
		"ps_mul		%7, %7, %9\n"			// m32_m33 / shift
		"psq_st		%7, 56(%8), 0, 0\n"		// store m32_m33
		: "=&f" (m00_m01), "=&f" (m02_m03),	// 0-1
		  "=&f" (m10_m11), "=&f" (m12_m13), // 2-3
		  "=&f" (m20_m21), "=&f" (m22_m23), // 4-5
		  "=&f" (m30_m31), "=&f" (m32_m33)  // 6-7
		: "b" (m), "f" (shift)				// 8-9
		: "memory"
	);
	/*
	__asm__ __volatile__ (
		"psq_l		%0,  0(%8), 0, 0\n"		// m00_m01
		"psq_l		%1,  8(%8), 0, 0\n"		// m02_m03
		"psq_l		%2, 16(%8), 0, 0\n"		// m10_m11
		"psq_l		%3, 24(%8), 0, 0\n"		// m12_m13
		"psq_l		%4, 32(%8), 0, 0\n"		// m20_m21
		"psq_l		%5, 40(%8), 0, 0\n"		// m22_m23
		"psq_l		%6, 48(%8), 0, 0\n"		// m30_m31
		"psq_l		%7, 56(%8), 0, 0\n"		// m32_m33


		"ps_mul		%0, %0, %9\n"			// m00_m01 / shift
		"ps_mul		%1, %1, %9\n"			// m02_m03 / shift
		"ps_mul		%2, %2, %9\n"			// m10_m11 / shift
		"ps_mul		%3, %3, %9\n"			// m12_m13 / shift
		"ps_mul		%4, %4, %9\n"			// m20_m21 / shift
		"ps_mul		%5, %5, %9\n"			// m22_m23 / shift
		"ps_mul		%6, %6, %9\n"			// m30_m31 / shift
		"ps_mul		%7, %7, %9\n"			// m32_m33 / shift

		"psq_st		%0,  0(%8), 0, 0\n"		// m00_m01
		"psq_st		%1,  8(%8), 0, 0\n"		// m02_m03
		"psq_st		%2, 16(%8), 0, 0\n"		// m10_m11
		"psq_st		%3, 24(%8), 0, 0\n"		// m12_m13
		"psq_st		%4, 32(%8), 0, 0\n"		// m20_m21
		"psq_st		%5, 40(%8), 0, 0\n"		// m22_m23
		"psq_st		%6, 48(%8), 0, 0\n"		// m30_m31
		"psq_st		%7, 56(%8), 0, 0\n"		// m32_m33
		
		
		
		: "=&f" (m00_m01), "=&f" (m02_m03),	// 0-1
		  "=&f" (m10_m11), "=&f" (m12_m13), // 2-3
		  "=&f" (m20_m21), "=&f" (m22_m23), // 4-5
		  "=&f" (m30_m31), "=&f" (m32_m33), // 6-7
		: "b" (m), "f" (shift)				// 8-9
		: "memory"
	);
	//*/
}


#endif
		