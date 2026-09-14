#ifndef __GUDESMUME_H__
#define __GUDESMUME_H__

#include <gctypes.h>


#ifdef ENABLE_PAIRED_SINGLE

extern "C" {
	void ps_MatrixMultVec4x4(register const float *matrix, register float *vecPtr);
	void ps_MatrixMultVec3x3(register float *matrix, register float *vecPtr);
	void ps_MatrixCopy(register float* matrixDST, register const float* matrixSRC);
	void ps_MatrixTranslate(register float *matrix, register float *ptr);
	void ps_MatrixScale(register float *matrix, register float *ptr);
	void ps_MatrixMultiply(register float* matrix, register float* rightMatrix);

	void ps_mtx_fix2float4x4(register f32* matrix, register const f32 divisor);
	void ps_mtx_fix2float3x4(register f32* matrix, register const f32 divisor);

	// Non-Matrix functions	
	void texCoordTrans(register f32* outST, register f32* mtxCurrent,
							register f32* coord, register f32* inST);
	void setFinalColorCombine(register u8* out, register u8* in);
}

#else //No paired single

void texCoordTrans(f32* outST, f32* mtxCurrent, f32* coord, f32* inST){
	outST[0] =((coord[0]*mtxCurrent[0] +
				coord[1]*mtxCurrent[4] +
				coord[2]*mtxCurrent[8]) + inST[0] * 16.0f) / 16.0f;
	outST[1] =((coord[0]*mtxCurrent[1] +
				coord[1]*mtxCurrent[5] +
				coord[2]*mtxCurrent[9]) + inST[1] * 16.0f) / 16.0f;
			
}

void setFinalColorCombine(u8* out, u8* in){
	u8 red = out[3];
	u8 green = out[2];
	u8 blue = out[1];
	u8 alpha = out[0];
	out[3] = ((red * alpha) + ((in[3]<<1) * (32 - alpha)))>>6;
	out[2] = ((green * alpha) + ((in[2]<<1) * (32 - alpha)))>>6;
	out[1] = ((blue * alpha) + ((in[1]<<1) * (32 - alpha)))>>6;
	out[0] = ((alpha * alpha) + ((in[0]<<1) * (32 - alpha)))>>6;
}


#endif


#endif