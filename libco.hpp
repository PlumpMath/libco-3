#pragma once
#include <cassert>
#include <stdexcept>
#include <functional>
#include <unordered_map>

namespace libco
{
	class ITask;
	class IScheduler;
	typedef std::function<void(ITask*)> Routine;

	enum { invalid_socket = -1 };

	class ITask
	{
	public:
		virtual IScheduler* GetOwner() = 0;
	public: // basic
		virtual void Sleep(std::uint64_t ms) = 0;
	public: // Berkeley socket
		virtual uv_os_sock_t socket(int af, int type, int protocol) = 0;
		virtual int closesocket(uv_os_sock_t s) = 0;
		virtual int connect(uv_os_sock_t s, const struct sockaddr* name, int namelen) = 0;
		virtual int send(uv_os_sock_t s, const char* buf, int len, int flags = 0) = 0;
		virtual int recv(uv_os_sock_t s, char* buf, int len, int flags = 0) = 0;
		/*
		virtual int bind(uv_os_sock_t s, const struct sockaddr* addr, int namelen) = 0;
		virtual int listen(uv_os_sock_t s, int backlog) = 0;
		virtual uv_os_sock_t accept(uv_os_sock_t s, struct sockaddr* addr, int* addrlen) = 0;
		virtual int shutdown(uv_os_sock_t s, int how) = 0;
	*/
	};

	class IScheduler
	{
	public:
		virtual void Delete() = 0;
	public:
		virtual bool Peek() = 0;
		virtual bool NewTask(Routine routine) = 0;
	};

	namespace impl
	{
		class IXTask;
		class IXScheduler;
		using FIBER_T = LPVOID;

		template<typename _Tn> void MemFree(_Tn* ptr) { free(ptr); }
		template<class _Tc, typename _Tn> _Tc* MemAlloc(_Tn size) { return (_Tc*)malloc(size); }

		uv_handle_t* MemAllocHandle(uv_handle_type type)
		{
			assert(type > UV_UNKNOWN_HANDLE);
			assert(type < UV_HANDLE_TYPE_MAX);

			uv_handle_t* handle;
			auto hsize = uv_handle_size(type);
			assert(hsize > 0);
			handle = MemAlloc<uv_handle_t>(hsize);
			if (handle == nullptr)
			{
				throw std::runtime_error("Alloc uv handle error");
			}
			memset(handle, 0x00, hsize);
			return handle;
		}

		class CXHandle
		{
		public:
			CXHandle() : m_handle(nullptr)
			{

			}
			CXHandle(const CXHandle& other)
			{
				m_handle = other.m_handle;
			}
			CXHandle& operator=(const CXHandle& other)
			{
				if (this != &other)
				{
					m_handle = other.m_handle;
				}
				return *this;
			}
			template<typename _Tn>
			CXHandle(_Tn* any_handle)
			{
				assert(any_handle != nullptr);
				assert(typeid(any_handle->type) == typeid(uv_handle_type));
				assert(any_handle->type > UV_UNKNOWN_HANDLE);
				assert(any_handle->type < UV_HANDLE_TYPE_MAX);

				m_handle = (uv_handle_t*)any_handle;
			}
			CXHandle(uv_loop_t* loop, uv_handle_type type)
			{
				int errcode = -1;
				assert(loop != nullptr);
				assert(type > UV_UNKNOWN_HANDLE);
				assert(type < UV_HANDLE_TYPE_MAX);

				m_handle = MemAllocHandle(type);

				switch (type)
				{
				case UV_TIMER:
					errcode = uv_timer_init(loop, *this);
					break;
				case UV_TCP:
					errcode = uv_tcp_init(loop, *this);
					break;
				case UV_UDP:
					errcode = uv_udp_init(loop, *this);
					break;
				default:
					throw std::invalid_argument("Unsupported uv handle type");
					break;
				}
				if (errcode != 0)
				{
					MemFree(m_handle);
					m_handle = nullptr;
					throw std::runtime_error("Init uv handle error");
				}
			}
			virtual ~CXHandle() { }
		public:
			operator uv_handle_t*() const { return m_handle; }
			template<typename _Tn> operator _Tn*() const
			{
				_Tn* any_handle = (_Tn*)m_handle;

				// make sure cast to a uv handle
				assert(any_handle != nullptr);
				assert(typeid(any_handle->type) == typeid(uv_handle_type));
				return any_handle;
			}
		public:
			void Close()
			{
				assert(m_handle != nullptr);

				uv_close(m_handle, [](uv_handle_t* handle) { MemFree(handle); });
			}
			IXTask* GetXTask()
			{
				assert(m_handle != nullptr);
				assert(m_handle->data != nullptr);

				return (IXTask*)m_handle->data;
			}
			void SetXTask(IXTask* task)
			{
				assert(m_handle != nullptr);

				m_handle->data = task;
			}
		private:
			uv_handle_t* m_handle;
		};

		class IXTask : public ITask
		{
		public:
			virtual void Delete() = 0;
		public:
			virtual FIBER_T GetFiber() const = 0;
			virtual IXScheduler* GetXOwner() const = 0;
		};

		class IXScheduler : public IScheduler
		{
		public:
			virtual void FreeTask(IXTask* task) = 0;
		public:
			virtual FIBER_T GetFiber() const = 0;
			virtual uv_loop_t* GetLoopContext() const = 0;
		public: // socket register
			virtual int DeleteSocket(uv_os_sock_t s) = 0;
			virtual bool QuerySocket(uv_os_sock_t s, uv_tcp_t*& handle) = 0;
			virtual bool QuerySocket(uv_os_sock_t s, uv_udp_t*& handle) = 0;
			virtual uv_os_sock_t CreateSocket(int af, int type, int protocol) = 0;
		};

		class CXTask : public IXTask
		{
		protected:
			CXTask(IXScheduler* owner, Routine routine)
				: m_owner(owner), m_routine(routine)
			{
				m_fiber = CreateFiberEx(0, 0, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)_entry_point, this);
				if (m_fiber == nullptr)
					throw std::runtime_error("create fiber error.");
			}
			virtual ~CXTask()
			{
				assert(GetCurrentFiber() != GetFiber());

				DeleteFiber(m_fiber);
			}
		public:
			static IXTask* Create(IXScheduler* owner, Routine func)
			{
				try
				{
					return new CXTask(owner, func);
				}
				catch (std::runtime_error&)
				{
					return nullptr;
				}
			}
			virtual void Delete() override { delete this; }
		private:
			void Run() { m_routine(this); }
			static VOID WINAPI _entry_point(CXTask* task)
			{
				IXScheduler* scheduler = task->GetXOwner();

				task->Run();
				scheduler->FreeTask(task);
				SwitchToFiber(scheduler->GetFiber());
				assert(false); // do not back here
			}
		public:
			virtual FIBER_T GetFiber() const override { return m_fiber; }
			virtual IScheduler* GetOwner() override { return m_owner; }
			virtual IXScheduler* GetXOwner() const override { return m_owner; }
		public:
			virtual void Sleep(std::uint64_t ms) override
			{
				CXHandle sleep_handle(GetXOwner()->GetLoopContext(), UV_TIMER);

				sleep_handle.SetXTask(this);
				if (uv_timer_start(sleep_handle, _sleep_finished, ms, 0) != 0)
				{
					throw std::runtime_error("sleep error");
				}
				// switch to Scheduler
				SwitchToFiber(GetXOwner()->GetFiber());
				// come back, oh yeah !!!
				sleep_handle.Close();
			}
			static void _sleep_finished(uv_timer_t* handle)
			{
				CXHandle Handle(handle);
				IXTask* task = Handle.GetXTask();

				// back to task
				SwitchToFiber(task->GetFiber());
			}
		public: // socket
			virtual uv_os_sock_t socket(int af, int type, int protocol) override
			{
				return GetXOwner()->CreateSocket(af, type, protocol);
			}
			virtual int closesocket(uv_os_sock_t s) override
			{
				return GetXOwner()->DeleteSocket(s);
			}
			virtual int connect(uv_os_sock_t s, const struct sockaddr* name, int namelen) override
			{
				uv_tcp_t* tcp_handle;

				m_cur_data.conn_data.status = -1;
				assert(sizeof(struct sockaddr) == namelen);
				if (GetXOwner()->QuerySocket(s, tcp_handle))
				{
					uv_connect_t conn = { this };

					if (uv_tcp_connect(&conn, tcp_handle, name, _connect_finished) == 0)
					{
						SwitchToFiber(GetXOwner()->GetFiber());
					}
				}
				return m_cur_data.conn_data.status;
			}
			virtual int send(uv_os_sock_t s, const char* buf, int len, int flags) override
			{
				uv_tcp_t* tcp_handle;

				m_cur_data.send_data.status = -1;
				if (GetXOwner()->QuerySocket(s, tcp_handle))
				{
					uv_write_t req = { this };
					uv_buf_t uvbuf;

					uvbuf.len = len;
					uvbuf.base = (char*)buf;
					if (uv_write(&req, (uv_stream_t*)tcp_handle, &uvbuf, 1, _send_finished) == 0)
					{
						SwitchToFiber(GetXOwner()->GetFiber());
					}
				}
				return m_cur_data.send_data.status;
			}
			virtual int recv(uv_os_sock_t s, char* buf, int len, int flags) override
			{
				uv_tcp_t* tcp_handle;

				if (GetXOwner()->QuerySocket(s, tcp_handle))
				{
					/*
					m_cur_data.recv_data.buf = buf;
					m_cur_data.recv_data.buf_size = len;
					m_cur_data.recv_data.status = -1;
					if (uv_read_start((uv_stream_t*)tcp_handle, _recv_alloc, _recv_finished) == 0)
					{
						SwitchToFiber(GetXOwner()->GetFiber());
					}
					*/
				}
				return m_cur_data.recv_data.status;
			}
		protected:
			static void _connect_finished(uv_connect_t* req, int status)
			{
				CXTask* task = (CXTask*)req->data;

				task->m_cur_data.conn_data.status = status;
				SwitchToFiber(task->GetFiber());
			}
			static void _send_finished(uv_write_t* req, int status)
			{
				CXTask* task = (CXTask*)req->data;

				task->m_cur_data.send_data.status = status;
				SwitchToFiber(task->GetFiber());
			}
			static void _recv_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
			{
				/*
				CXTask* task = (CXTask*)handle->data;

				buf->base = task->m_cur_data.recv_data.buf;
				buf->len = task->m_cur_data.recv_data.buf_size;
				*/
			}
			static void _recv_finished(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
			{
				/*
				CXTask* task = (CXTask*)stream->data;

				task->m_cur_data.recv_data.status = nread;
				SwitchToFiber(task->GetFiber());
				*/
			}
		private:
			FIBER_T m_fiber;
			Routine m_routine;
			IXScheduler* m_owner;

			union
			{
				struct
				{
					int status;
				}conn_data;
				struct
				{
					int status;
				}send_data;
				struct
				{
					char* buf;
					int buf_size;

					int status;
				}recv_data;
			}m_cur_data;
		};

		class CXScheduler : public IXScheduler
		{
		protected:
			CXScheduler() : m_fiber(nullptr), m_loop_context(nullptr)
			{
				m_was_converted = (IsThreadAFiber() != FALSE);
				if (m_was_converted)
				{
					m_fiber = GetCurrentFiber();
				}
				else
				{
					m_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
				}
				assert(m_fiber != nullptr);

				size_t uv_size = uv_loop_size();
				assert(uv_size > 0);
				m_loop_context = (uv_loop_t*)malloc(uv_size);
				assert(m_loop_context != nullptr);
				memset(m_loop_context, 0x00, uv_size);
				int errcode = uv_loop_init(m_loop_context);
				assert(errcode == 0);
				m_loop_context->data = dynamic_cast<IXScheduler*>(this);
			}
			virtual ~CXScheduler()
			{
				assert(GetCurrentFiber() == GetFiber());

				do
				{
					Peek();
				} while (uv_loop_close(m_loop_context) == UV_EBUSY);
				free(m_loop_context);
				m_loop_context = nullptr;

				if (!m_was_converted)
				{
					ConvertFiberToThread();
				}
			}
		public:
			static IScheduler* Create() { return new CXScheduler(); }
			virtual void Delete() override { delete this; }
		public:
			virtual FIBER_T GetFiber() const override { return m_fiber; }
			virtual uv_loop_t* GetLoopContext() const override { return m_loop_context; }
		public:
			virtual bool Peek() override
			{
				return (uv_run(m_loop_context, UV_RUN_NOWAIT) == 0);
			}
			virtual bool NewTask(Routine func) override
			{
				if (IXTask* task = CXTask::Create(this, func))
				{
					CXHandle Handle(m_loop_context, UV_TIMER);

					Handle.SetXTask(task);
					if (uv_timer_start(Handle, _task_execute, 0, 0) == 0)
					{
						return true;
					}
					Handle.Close();
					task->Delete();
				}
				return false;
			}
			virtual void FreeTask(IXTask* task) override
			{
				CXHandle Handle(task->GetXOwner()->GetLoopContext(), UV_TIMER);

				Handle.SetXTask(task);
				if (uv_timer_start(Handle, _task_finished, 0, 0) != 0)
				{
					throw std::runtime_error("Free task error");
				}
			}
		protected:
			static void _task_execute(uv_timer_t* handle)
			{
				CXHandle Handle(handle);
				IXTask* task = Handle.GetXTask();

				Handle.Close();
				SwitchToFiber(task->GetFiber());
			}
			static void _task_finished(uv_timer_t* handle)
			{
				CXHandle Handle(handle);
				IXTask* task = Handle.GetXTask();

				Handle.Close();
				task->Delete();
			}
		public: // socket
			virtual uv_os_sock_t CreateSocket(int af, int type, int protocol) override
			{
				uv_os_sock_t sock = ::socket(af, type, protocol);

				if (sock == invalid_socket)
					return sock;

				if (type == SOCK_STREAM)
				{
					CXHandle handle(m_loop_context, UV_TCP);

					if (uv_tcp_open(handle, sock) == 0)
					{
						m_tcp_table[sock] = handle;
						return sock;
					}
					handle.Close();
				}
				else if (type == SOCK_DGRAM)
				{
					CXHandle handle(m_loop_context, UV_UDP);

					if (uv_udp_open(handle, sock) == 0)
					{
						m_tcp_table[sock] = handle;
						return sock;
					}
					handle.Close();
				}
				::closesocket(sock);
				return invalid_socket;
			}
			virtual int DeleteSocket(uv_os_sock_t s) override
			{
				uv_tcp_t* tcp_handle;
				uv_udp_t* udp_handle;

				if (QuerySocket(s, tcp_handle))
				{
					m_tcp_table.erase(s);
					CXHandle(tcp_handle).Close();
					return 0;
				}
				else if (QuerySocket(s, udp_handle))
				{
					m_udp_table.erase(s);
					CXHandle(udp_handle).Close();
					return 0;
				}
				return -1;
			}
			virtual bool QuerySocket(uv_os_sock_t s, uv_tcp_t*& handle) override
			{
				auto iter = m_tcp_table.find(s);
				if (iter != m_tcp_table.end())
				{
					handle = iter->second;
					return true;
				}
				return false;
			}
			virtual bool QuerySocket(uv_os_sock_t s, uv_udp_t*& handle) override
			{
				auto iter = m_udp_table.find(s);
				if (iter != m_udp_table.end())
				{
					handle = iter->second;
					return true;
				}
				return false;
			}
		private:
			FIBER_T m_fiber;
			bool m_was_converted;
			uv_loop_t* m_loop_context;

			std::unordered_map<uv_os_sock_t, uv_tcp_t*> m_tcp_table;
			std::unordered_map<uv_os_sock_t, uv_udp_t*> m_udp_table;
		};
	} // namespace impl

	IScheduler* CreateScheduler() { return impl::CXScheduler::Create(); }
}

