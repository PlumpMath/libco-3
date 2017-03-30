#include "stdafx.h"
#define USING_UV_SHARED
#include "libuv/uv.h"
#pragma comment(lib, "libuv.lib")
#include "libco.hpp"
#include <thread>
#include <system_error>

// task in task
void task_func(libco::ITask* task)
{
	task->GetOwner()->NewTask([](auto* task) {
		printf("%s\n", __FUNCTION__);
	});
}

void sample()
{
	auto* scheduler = libco::CreateScheduler();

	scheduler->NewTask(task_func);

	scheduler->Peek();
	scheduler->Delete();
}

// scheduler in scheduler
void sample_nesting()
{
	auto* scheduler = libco::CreateScheduler();

	scheduler->NewTask([](auto* task) {
		sample();
	});

	scheduler->Peek();
	scheduler->Delete();
}

int main()
{
	auto* scheduler = libco::CreateScheduler();

	for (int i = 0 ; i < 10000; i++)
	{
		bool ok = scheduler->NewTask([](auto* task) {
			
		});
		if (!ok)
		{
			try
			{
				std::thread([] {
					auto* scheduler = libco::CreateScheduler();

					for (int i = 0; i < 10000; i++)
					{
						bool ok = scheduler->NewTask([](auto* task) {

						});

						if (!ok)
						{
							GetTickCount();
						}
					}
				}).join();
			}
			catch (std::system_error& e)
			{
				GetTickCount();
			}
			
		}
	}



    return 0;
}

