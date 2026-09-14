/*  Copyright (C) 2006-2007 shash
	Copyright (C) 2009 DeSmuME team
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
#ifndef MATRIX_H
#define MATRIX_H

#include <math.h>
#include <string.h>

#include "types.h"
#include "mem.h"

#ifdef ENABLE_PAIRED_SINGLE
#include "guDesmume.h"
#endif

struct MatrixStack
{
	MatrixStack(int size);
	float	*matrix;
	s32		position;
	s32		size;
};

void	MatrixInit					(float *matrix);
float	MatrixGetMultipliedIndex	(int index, float *matrix, float *rightMatrix);
void	MatrixSet					(float *matrix, int x, int y, float value);
int		MatrixCompare				(const float * matrixDST, const float * matrixSRC);
void	MatrixIdentity				(float *matrix);

void	MatrixTranspose				(float *matrix);
void	MatrixStackInit				(MatrixStack *stack);
void	MatrixStackSetMaxSize		(MatrixStack *stack, int size);
void	MatrixStackSetStackPosition (MatrixStack *stack, int pos);
void	MatrixStackPushMatrix		(MatrixStack *stack, const float *ptr);
float*	MatrixStackPopMatrix		(MatrixStack *stack, int size);
float*	MatrixStackGetPos			(MatrixStack *stack, int pos);
float*	MatrixStackGet				(MatrixStack *stack);
void	MatrixStackLoadMatrix		(MatrixStack *stack, int pos, const float *ptr);

//---------------------------
#ifdef ENABLE_PAIRED_SINGLE
#define MatrixMultVec4x4  ps_MatrixMultVec4x4
#define MatrixMultVec3x3  ps_MatrixMultVec3x3
#define MatrixMultiply    ps_MatrixMultiply
#define MatrixTranslate   ps_MatrixTranslate
#define MatrixScale       ps_MatrixScale
#define MatrixCopy        ps_MatrixCopy

#else // c versions

void MatrixMultVec4x4 (const float *matrix, float *vecPtr);
void MatrixMultVec3x3(const float * matrix, float * vecPtr);
void MatrixMultiply(float * matrix, const float * rightMatrix);
void MatrixTranslate(float *matrix, const float *ptr);
void MatrixScale(float * matrix, const float * ptr);
void MatrixCopy(float * matrixDST, const float * matrixSRC);

#endif

FORCEINLINE void mtx_fix2float4x4(float* matrix, const float divisor);
FORCEINLINE void mtx_fix2float3x4(float* matrix, const float divisor);

FORCEINLINE void MatrixMultVec4x4_M2(const float *matrix, float *vecPtr)
{
	//there are hardly any gains from merging these manually
	MatrixMultVec4x4(matrix+16,vecPtr);
	MatrixMultVec4x4(matrix,vecPtr);
}

template<int NUM_ROWS>
FORCEINLINE void vector_fix2float(float* matrix, const float divisor)
{
	for(int i=0;i<NUM_ROWS*4;i++)
		matrix[i] /= divisor;
}

template<int NUM>
static FORCEINLINE void memset_u8(void* dst, u8 val)
{
	memset(dst,val,NUM);
}




#endif
