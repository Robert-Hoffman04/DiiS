/*  Copyright 2009 DeSmuME team
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

#ifndef _TASK_H_

//Sort of like a single-thread thread pool.
//You hand it a worker function and then call finish() to synch with its completion
class Task
{
public:
	Task();
	~Task();
	
	typedef void * (*TWork)(void *);

	// initialize task runner
	void start(bool spinlock);

	//execute some work
	void execute(const TWork &work, void* param);

	//wait for the work to complete
	void* finish();

	// does the opposite of start
	void shutdown();

	class Impl;
	Impl *impl;

};


#endif
