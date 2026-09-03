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
#ifndef _VALUEARRAY_H_
#define _VALUEARRAY_H_

template<typename T, int N>
struct ValueArray
{
	T data[N];
	T &operator[](int index) { return data[index]; }
	static const int size = N;
	bool operator!=(ValueArray<T,N> &other) { return !operator==(other); }
	bool operator==(ValueArray<T,N> &other)
	{
		for(int i=0;i<size;i++)
			if(data[i] != other[i])
				return false;
		return true;
	}
};

#endif

