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
		virtual bool Sleep(std::uint64_t ms) = 0;
	public: // socket
		virtual uv_os_sock_t socket(int af, int type = SOCK_STREAM, int protocol = IPPROTO_TCP) = 0;
		virtual int closesocket(uv_os_sock_t s) = 0;
		virtual int connect(uv_os_sock_t s, const struct sockaddr* name, int namelen) = 0;
		virtual int send(uv_os_sock_t s, const char* buf, int len) = 0;
		virtual int recv(uv_os_sock_t s, char* buf, int len) = 0;
		virtual int shutdown(uv_os_sock_t s) = 0;
		virtual int bind(uv_os_sock_t s, const struct sockaddr* addr, int namelen) = 0;
		virtual int listen(uv_os_sock_t s, int backlog) = 0;
		virtual uv_os_sock_t accept(uv_os_sock_t s, struct sockaddr* addr, int* addrlen) = 0;
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
		template<class _Tc, typename _Tn> _Tc* MemAlloc(_Tn size)
		{
			_Tc* ptr = (_Tc*)malloc(size);

			if (ptr == nullptr)
			{
				throw std::runtime_error("Allocate memory error");
			}
			memset(ptr, 0x00, size);
			return ptr;
		}

		class CXHandle
		{
		public:
			CXHandle() : m_handle(nullptr)
			{

			}
			CXHandle(const CXHandle& other)
			{
				assert(other.m_handle != nullptr);
				assert(other.m_handle->data != nullptr);

				m_handle = other.m_handle;
			}
			CXHandle& operator=(const CXHandle& other)
			{
				if (this != &other)
				{
					assert(other.m_handle != nullptr);
					assert(other.m_handle->data != nullptr);

					m_handle = other.m_handle;
				}
				return *this;
			}
			template<typename _Tn>
			CXHandle(_Tn* any_handle)
			{
				assert(any_handle != nullptr);
				assert(any_handle->data != nullptr);
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

				m_handle = AllocHandle(type);

				switch (type)
				{
				case UV_TIMER:
					errcode = uv_timer_init(loop, *this);
					break;
				case UV_TCP:
					errcode = uv_tcp_init(loop, *this);
					break;
				default:
					throw std::invalid_argument("Unsupported uv handle type");
					break;
				}
				if (errcode != 0)
				{
					FreeHandle(m_handle);
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
				assert(any_handle->data != nullptr);
				assert(typeid(any_handle->type) == typeid(uv_handle_type));
				return any_handle;
			}
		protected:
			typedef struct
			{
				IXTask* owner;
				void* exclude; // only one task can use it in the same time
			}HCONTEXT;
			static void FreeHandle(uv_handle_t* handle)
			{
				CXHandle Handle(handle);
				auto* hctx = Handle.GetHandleContext();

				if (hctx->exclude != nullptr)
				{
					MemFree(hctx->exclude);
				}
				MemFree(hctx);
				MemFree(handle);
			}
			static uv_handle_t* AllocHandle(uv_handle_type type)
			{
				assert(type > UV_UNKNOWN_HANDLE);
				assert(type < UV_HANDLE_TYPE_MAX);

				auto hsize = uv_handle_size(type);
				assert(hsize > 0);
				uv_handle_t* handle = MemAlloc<uv_handle_t>(hsize);
				handle->data = MemAlloc<HCONTEXT>(sizeof(HCONTEXT));
				return handle;
			}
			HCONTEXT* GetHandleContext()
			{
				assert(m_handle != nullptr);
				assert(m_handle->data != nullptr);

				return (HCONTEXT*)m_handle->data;
			}
		public:
			void Close()
			{
				assert(m_handle != nullptr);

				uv_close(m_handle, [](uv_handle_t* handle) { FreeHandle(handle); });
			}
		public: // handle's owner
			IXTask* GetXTask()
			{
				auto* ctx = GetHandleContext();

				assert(ctx->owner != nullptr);
				return ctx->owner;
			}
			void SetXTask(IXTask* task)
			{
				auto* ctx = GetHandleContext();

				assert(ctx->owner == nullptr);
				ctx->owner = task;
			}
		public: // only for socket handle
			template<class _Tn>
			_Tn* GetExclude()
			{
				assert(m_handle->type == UV_TCP);

				auto* ctx = GetHandleContext();

				assert(ctx->exclude != nullptr);
				return (_Tn*)ctx->exclude;
			}
			bool SetExclude(void* object)
			{
				assert(m_handle->type == UV_TCP);

				bool ok;
				auto* ctx = GetHandleContext();

				// only one task can recv in the same time
				if (ok = (ctx->exclude == nullptr))
				{
					ctx->exclude = object;
				}
				return ok;
			}
			void ResetExclude()
			{
				assert(m_handle->type == UV_TCP);

				auto* ctx = GetHandleContext();

				ctx->exclude = nullptr;
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
			virtual uv_os_sock_t CreateTcpSocket(int af) = 0;
			virtual bool AttachTcpSocket(uv_os_sock_t s, uv_tcp_t* uv_handle = nullptr) = 0;
			virtual bool DetachTcpSocket(uv_os_sock_t s) = 0;
			virtual uv_tcp_t* QueryTcpSocket(uv_os_sock_t s) = 0;
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
			virtual bool Sleep(std::uint64_t ms) override
			{
				CXHandle sleep_handle(GetXOwner()->GetLoopContext(), UV_TIMER);

				sleep_handle.SetXTask(this);
				int errcode = uv_timer_start(sleep_handle, [](uv_timer_t* handle) {
					CXHandle Handle(handle);
					IXTask* task = Handle.GetXTask();

					// back to task
					SwitchToFiber(task->GetFiber());
				}, ms, 0);
				if (errcode == 0)
				{
					// switch to Scheduler
					SwitchToFiber(GetXOwner()->GetFiber());
					// come back, oh yeah !!!
				}
				sleep_handle.Close();
				return (errcode == 0);
			}
		protected: // socket io struct ext
			enum uv_exclude_type { uv_exclude_none, uv_exclude_recv, uv_exclude_listen };
			struct uv_exclude_ext { uv_exclude_type type; };
			struct uv_conn_ext : uv_connect_t { IXTask* task; int status; };
			struct uv_send_ext : uv_write_t { IXTask* task; int status; };
			struct uv_recv_ext : uv_exclude_ext { IXTask* task; char* buf; int len; ssize_t nread; };
			struct uv_shutdown_ext : uv_shutdown_t { IXTask* task; int status; };
			struct uv_listen_ext : uv_exclude_ext { IXTask* task; int last_status; int queue_count; };
		public: // socket
			virtual uv_os_sock_t socket(int af, int type, int protocol) override
			{
				if (type == SOCK_STREAM)
				{
					if ((protocol == IPPROTO_TCP) || (protocol == 0))
					{
						return GetXOwner()->CreateTcpSocket(af);
					}
				}
				return invalid_socket;
			}
			virtual int closesocket(uv_os_sock_t s) override
			{
				return GetXOwner()->DetachTcpSocket(s);
			}
			virtual int connect(uv_os_sock_t s, const struct sockaddr* name, int namelen) override
			{
				int status = -1;
				
				assert(sizeof(struct sockaddr) == namelen);
				if (uv_tcp_t* tcp_handle = GetXOwner()->QueryTcpSocket(s))
				{
					int errcode;
					uv_conn_ext reqx;

					reqx.task = this;
					reqx.status = status;
					errcode = uv_tcp_connect(&reqx, tcp_handle, name, [](uv_connect_t* req, int status) {
						uv_conn_ext* reqx = (uv_conn_ext*)req;

						reqx->status = status;
						SwitchToFiber(reqx->task->GetFiber());
					});
					if (errcode == 0)
					{
						SwitchToFiber(GetXOwner()->GetFiber());
						status = reqx.status;
					}
				}
				return status;
			}
			virtual int send(uv_os_sock_t s, const char* buf, int len) override
			{
				int status = -1;

				if (uv_tcp_t* tcp_handle = GetXOwner()->QueryTcpSocket(s))
				{
					int errcode;
					uv_buf_t uvbuf;
					uv_send_ext reqx;

					uvbuf.len = len;
					uvbuf.base = (char*)buf;
					reqx.task = this;
					reqx.status = status;
					errcode = uv_write(&reqx, (uv_stream_t*)tcp_handle, &uvbuf, 1, [](uv_write_t* req, int status) {
						uv_send_ext* reqx = (uv_send_ext*)req;

						reqx->status = status;
						SwitchToFiber(reqx->task->GetFiber());
					});
					if (errcode == 0)
					{
						SwitchToFiber(GetXOwner()->GetFiber());
						status = reqx.status;
					}
				}
				return status;
			}
			virtual int recv(uv_os_sock_t s, char* buf, int len) override
			{
				int status = -1;

				if (uv_tcp_t* tcp_handle = GetXOwner()->QueryTcpSocket(s))
				{
					uv_recv_ext reqx;
					CXHandle Handle(tcp_handle);

					reqx.type = uv_exclude_recv;
					reqx.task = this;
					reqx.buf = buf;
					reqx.len = len;
					reqx.nread = status;
					if (Handle.SetExclude(&reqx))
					{
						int errcode = uv_read_start(Handle, [](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
							CXHandle Handle(handle);
							uv_recv_ext* reqx = Handle.GetExclude<uv_recv_ext>();

							assert(reqx->type == uv_exclude_recv);
							buf->base = reqx->buf;
							buf->len = reqx->len;
						}, [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
							CXHandle Handle(stream);
							uv_recv_ext* reqx = Handle.GetExclude<uv_recv_ext>();

							assert(reqx->type == uv_exclude_recv);
							reqx->nread = nread;
							uv_read_stop(Handle);
							SwitchToFiber(reqx->task->GetFiber());
						});
						if (errcode == 0)
						{
							SwitchToFiber(GetXOwner()->GetFiber());
							status = reqx.nread;
						}
						Handle.ResetExclude();
					}
				}
				return status;
			}
			virtual int shutdown(uv_os_sock_t s) override
			{
				if (uv_tcp_t* tcp_handle = GetXOwner()->QueryTcpSocket(s))
				{
					uv_shutdown_ext reqx;

					reqx.task = this;
					reqx.status = -1;
					int errcode = uv_shutdown(&reqx, CXHandle(tcp_handle), [](uv_shutdown_t* req, int status) {
						uv_shutdown_ext* reqx = (uv_shutdown_ext*)req;

						reqx->status = status;
						SwitchToFiber(reqx->task->GetFiber());
					});
					if (errcode == 0)
					{
						SwitchToFiber(GetXOwner()->GetFiber());
						errcode = reqx.status;
					}
					return errcode;
				}
				return -1;
			}
			virtual int bind(uv_os_sock_t s, const struct sockaddr* addr, int namelen) override
			{
				if (uv_tcp_t* tcp_handle = GetXOwner()->QueryTcpSocket(s))
				{
					return uv_tcp_bind(tcp_handle, addr, 0);
				}
				return -1;
			}
			virtual int listen(uv_os_sock_t s, int backlog) override
			{
				if (uv_tcp_t* tcp_handle = GetXOwner()->QueryTcpSocket(s))
				{
					CXHandle Handle(tcp_handle);
					uv_listen_ext* reqx = MemAlloc<uv_listen_ext>(sizeof(uv_listen_ext));

					reqx->type = uv_exclude_listen;
					reqx->task = nullptr;
					reqx->last_status = 0;
					reqx->queue_count = 0;
					if (Handle.SetExclude(reqx))
					{
						return uv_listen(Handle, backlog, [](uv_stream_t* server, int status) {
							CXHandle Handle(server);
							uv_listen_ext* reqx = Handle.GetExclude<uv_listen_ext>();

							assert(reqx != nullptr);
							if (reqx->type == uv_exclude_listen)
							{
								if (status == 0)
								{
									reqx->queue_count++;
								}
								reqx->last_status = status;
								if (reqx->task != nullptr)
								{
									SwitchToFiber(reqx->task->GetFiber());
								}
							}
							else
							{
								assert(false);
							}
						});
					}
				}
				return -1;
			}
			virtual uv_os_sock_t accept(uv_os_sock_t s, struct sockaddr* addr, int* addrlen) override
			{
				if (uv_tcp_t* tcp_handle = GetXOwner()->QueryTcpSocket(s))
				{
					CXHandle server(tcp_handle);
					uv_listen_ext* reqx = server.GetExclude<uv_listen_ext>();

					auto _accept_stub = [this](CXHandle& server) -> uv_os_sock_t {
						CXHandle client(GetXOwner()->GetLoopContext(), UV_TCP);

						if (uv_accept(server, client) == 0)
						{
							uv_tcp_t* uv_tcp_client = client;
							uv_os_sock_t uv_os_client = uv_tcp_client->socket;

							if (GetXOwner()->AttachTcpSocket(uv_os_client, uv_tcp_client))
							{
								return uv_os_client;
							}
						}
						client.Close();
						return invalid_socket;
					};

					if (((reqx != nullptr)) && (reqx->type == uv_exclude_listen))
					{
						if (reqx->last_status == 0)
						{
							while (reqx->queue_count > 0)
							{
								reqx->queue_count--;
								uv_os_sock_t sock_client = _accept_stub(server);

								if (sock_client != invalid_socket)
								{
									return sock_client;
								}
							}
							// no client coming
							// wait for listen_callback wake up me
							reqx->task = this;
							SwitchToFiber(GetXOwner()->GetFiber());
							reqx->task = nullptr;
							if (reqx->last_status == 0)
							{
								reqx->queue_count--;
								assert(reqx->queue_count == 0);
								uv_os_sock_t sock_client = _accept_stub(server);

								if (sock_client != invalid_socket)
								{
									return sock_client;
								}
							}
						}
					}
				}
				return invalid_socket;
			}
		private:
			FIBER_T m_fiber;
			Routine m_routine;
			IXScheduler* m_owner;
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

				size_t loop_size = uv_loop_size();
				assert(loop_size > 0);
				m_loop_context = MemAlloc<uv_loop_t>(loop_size);
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
				MemFree(m_loop_context);
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
				return (uv_run(GetLoopContext(), UV_RUN_NOWAIT) == 0);
			}
			virtual bool NewTask(Routine func) override
			{
				if (IXTask* task = CXTask::Create(this, func))
				{
					CXHandle Handle(GetLoopContext(), UV_TIMER);

					Handle.SetXTask(task);
					int errcode = uv_timer_start(Handle, [](uv_timer_t* handle) {
						CXHandle Handle(handle);
						IXTask* task = Handle.GetXTask();

						Handle.Close();
						SwitchToFiber(task->GetFiber());
					}, 0, 0);
					if (errcode == 0)
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
				int errcode = uv_timer_start(Handle, [](uv_timer_t* handle) {
					CXHandle Handle(handle);
					IXTask* task = Handle.GetXTask();

					Handle.Close();
					task->Delete();
				}, 0, 0);
				if (errcode != 0)
				{
					throw std::runtime_error("Free task error");
				}
			}
		public: // socket
			virtual uv_os_sock_t CreateTcpSocket(int af) override
			{
				uv_os_sock_t sock = ::socket(af, SOCK_STREAM, IPPROTO_TCP);

				if (sock != invalid_socket)
				{
					if (AttachTcpSocket(sock))
					{
						return sock;
					}
					::closesocket(sock);
				}
				return invalid_socket;
			}
			virtual bool AttachTcpSocket(uv_os_sock_t s, uv_tcp_t* uv_handle = nullptr) override
			{
				if (QueryTcpSocket(s) == nullptr)
				{
					if (uv_handle != nullptr)
					{
						m_tcp_table[s] = uv_handle;
						return true;
					}
					else
					{
						CXHandle handle(GetLoopContext(), UV_TCP);

						if (uv_tcp_open(handle, s) == 0)
						{
							m_tcp_table[s] = handle;
							return true;
						}
						handle.Close();
					}
				}
				return false;
			}
			virtual bool DetachTcpSocket(uv_os_sock_t s) override
			{
				if (uv_tcp_t* tcp_handle = QueryTcpSocket(s))
				{
					m_tcp_table.erase(s);
					CXHandle(tcp_handle).Close();
					return true;
				}
				return false;
			}
			virtual uv_tcp_t* QueryTcpSocket(uv_os_sock_t s) override
			{
				auto tcp_iter = m_tcp_table.find(s);
				if (tcp_iter != m_tcp_table.end())
				{
					return tcp_iter->second;
				}
				return nullptr;
			}
		private:
			FIBER_T m_fiber;
			bool m_was_converted;
			uv_loop_t* m_loop_context;

			std::unordered_map<uv_os_sock_t, uv_tcp_t*> m_tcp_table;
		};
	} // namespace impl

	IScheduler* CreateScheduler() { return impl::CXScheduler::Create(); }
}

