#include "stdafx.h"
#define USING_UV_SHARED
#include "libuv/uv.h"
#pragma comment(lib, "libuv.lib")
#include "libco.hpp"
#include <thread>
#pragma comment(lib, "ws2_32.lib")

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

	scheduler->NewTask([](libco::ITask* task) {
		int err;
		SOCKET sock;
		sockaddr_in dest;
		sock = task->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		err = uv_ip4_addr("127.0.0.1", 3333, &dest);
		err = task->connect(sock, (sockaddr*)&dest, sizeof(dest));
		err = task->send(sock, "lll", 3, 0);
		char rrr[32] = {0};
		err = task->recv(sock, rrr, 32, 0);

		task->closesocket(sock);
	});

	scheduler->Delete();
    return 0; 
}

