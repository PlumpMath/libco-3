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

void tcp_server_responder(libco::ITask* task, SOCKET sock)
{
	char buf[256];

	while (true)
	{
		int n = task->recv(sock, buf, 256);

		if (n > 0)
		{
			task->send(sock, buf, n);
		}
		else
		{
			break;
		}
	}
	task->closesocket(sock);
}

void tcp_server(libco::ITask* task)
{
	int err;
	SOCKET server;
	sockaddr_in dest;

	server = task->socket(AF_INET);
	err = uv_ip4_addr("127.0.0.1", 6666, &dest);
	err = task->bind(server, (sockaddr*)&dest, sizeof(dest));
	err = task->listen(server, 100000);

	while (true)
	{
		SOCKET cli = task->accept(server, nullptr, nullptr);

		if (cli != INVALID_SOCKET)
		{
			task->GetOwner()->NewTask(std::bind(tcp_server_responder, std::placeholders::_1, cli));
		}
		else
		{
			break;
		}
	}
	task->closesocket(server);
}

int main()
{
	auto* scheduler = libco::CreateScheduler();

	scheduler->NewTask(tcp_server);

	scheduler->Peek();
	scheduler->Delete();
    return 0; 
}

