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
#ifndef BITS_H
#define BITS_H

#define BIT(n)  (1<<(n))

#define BIT_N(i,n)  (((i)>>(n))&1)
#define BIT0(i)     ((i)&1)
#define BIT1(i)     BIT_N(i,1)
#define BIT2(i)     BIT_N(i,2)
#define BIT3(i)     BIT_N(i,3)
#define BIT4(i)     BIT_N(i,4)
#define BIT5(i)     BIT_N(i,5)
#define BIT6(i)     BIT_N(i,6)
#define BIT7(i)     BIT_N(i,7)
#define BIT8(i)     BIT_N(i,8)
#define BIT9(i)     BIT_N(i,9)
#define BIT10(i)     BIT_N(i,10)
#define BIT11(i)     BIT_N(i,11)
#define BIT12(i)     BIT_N(i,12)
#define BIT13(i)     BIT_N(i,13)
#define BIT14(i)     BIT_N(i,14)
#define BIT15(i)     BIT_N(i,15)
#define BIT16(i)     BIT_N(i,16)
#define BIT17(i)     BIT_N(i,17)
#define BIT18(i)     BIT_N(i,18)
#define BIT19(i)     BIT_N(i,19)
#define BIT20(i)     BIT_N(i,20)
#define BIT21(i)     BIT_N(i,21)
#define BIT22(i)     BIT_N(i,22)
#define BIT23(i)     BIT_N(i,23)
#define BIT24(i)     BIT_N(i,24)
#define BIT25(i)     BIT_N(i,25)
#define BIT26(i)     BIT_N(i,26)
#define BIT27(i)     BIT_N(i,27)
#define BIT28(i)     BIT_N(i,28)
#define BIT29(i)     BIT_N(i,29)
#define BIT30(i)     BIT_N(i,30)
#define BIT31(i)    ((i)>>31)

#define CONDITION(i)  (i)>>28

#define REG_POS(i,n)         (((i)>>n)&0xF)

#endif
