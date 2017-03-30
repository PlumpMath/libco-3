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
		virtual IScheduler* GetOwner() const = 0;
	public: // basic
		virtual void Sleep(std::uint64_t ms) = 0;
	public: // Berkeley socket
		virtual uv_os_sock_t socket(int af, int type, int protocol) = 0;
		virtual int bind(uv_os_sock_t s, const struct sockaddr* addr, int namelen) = 0;
		virtual int listen(uv_os_sock_t s, int backlog) = 0;
		virtual uv_os_sock_t accept(uv_os_sock_t s, struct sockaddr* addr, int* addrlen) = 0;
		virtual int connect(uv_os_sock_t s, const struct sockaddr* name, int namelen) = 0;
		virtual int send(uv_os_sock_t s, const char* buf, int len, int flags) = 0;
		virtual int recv(uv_os_sock_t s, char* buf, int len, int flags) = 0;
		virtual int shutdown(uv_os_sock_t s, int how) = 0;
		virtual int closesocket(uv_os_sock_t s) = 0;
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

		template<typename _To> void MemFree(_To* ptr) { free(ptr); }
		template<class _To, typename _Ts> _To* MemAlloc(_Ts size) { return (_To*)malloc(size); }

		class CXHandle
		{
		public:
			CXHandle()
				: m_handle(nullptr)
			{

			}
			CXHandle(uv_handle_t* handle, bool incr_ref = true)
			{
				assert(handle != nullptr);
				assert(handle->type != UV_UNKNOWN_HANDLE);
				if (incr_ref)
					uv_ref(handle);
				m_handle = handle;
			}
			virtual ~CXHandle()
			{
				if (m_handle != nullptr)
				{
					uv_unref(m_handle);
					if (!uv_has_ref(m_handle))
					{
						uv_close(m_handle, _handle_closed);
					}
				}
			}
		public:
			operator uv_handle_t*() const { return m_handle; }
			template<typename _Tv>
			operator _Tv*() const
			{
				_Tv* ptr = (_Tv*)m_handle;

				// make sure cast to a handle
				assert(typeid(ptr->data) == typeid(void*));
				assert(typeid(ptr->loop) == typeid(uv_loop_t*));
				assert(typeid(ptr->type) == typeid(uv_handle_type));
				return ptr;
			}
		protected:
			static void _handle_closed(uv_handle_t* handle) { MemFree(handle); }
		private:
			uv_handle_t* m_handle;
		};

		class CHandle
		{
		public:
			CHandle(uv_handle_type h_type, IXTask* task = nullptr)
				: m_handle(Alloc(h_type))
			{
				m_handle->data = task;
			}
			CHandle(uv_handle_t* handle = nullptr) : m_handle(handle) { }
			CHandle(uv_timer_t* handle) : CHandle((uv_handle_t*)handle) { }
			CHandle(uv_tcp_t* handle) : CHandle((uv_handle_t*)handle) { }
			CHandle(uv_udp_t* handle) : CHandle((uv_handle_t*)handle) { }
		protected:
			static void Free(uv_handle_t* handle) { free(handle); }
			static uv_handle_t* Alloc(uv_handle_type handle_type)
			{
				auto hsize = uv_handle_size(handle_type);
				assert(hsize > 0);
				uv_handle_t* handle = (uv_handle_t*)malloc(hsize);
				assert(handle != nullptr);
				memset(handle, 0x00, hsize);
				return handle;
			}
			static void _handle_closed(uv_handle_t* handle) { Free(handle); }
		public:
			bool IsNull() const { return (m_handle != nullptr); }
			void Close()
			{
				assert(!IsNull());
				
				if (m_handle->type == UV_UNKNOWN_HANDLE)
				{
					Free(m_handle);
				}
				else
				{
					uv_close(m_handle, _handle_closed);
				}
			}
			IXTask* GetXTask() const
			{
				assert(!IsNull());
				return (IXTask*)m_handle->data;
			}
		public:
			operator uv_handle_t*() const { return m_handle; }
			operator uv_timer_t*() const { return (uv_timer_t*)m_handle; }
			operator uv_tcp_t*() const { return (uv_tcp_t*)m_handle; }
			operator uv_udp_t*() const { return (uv_udp_t*)m_handle; }
		private:
			uv_handle_t* m_handle;
		};

		typedef struct
		{
			uv_os_sock_t s;
			int sock_type;
			CHandle handle;
		}socket_object;

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
		public: // socket support
			virtual bool NewSocket(socket_object& so, int af, int type, int protocol) = 0;
			virtual bool FreeSocket(uv_os_sock_t s) = 0;
			virtual bool QuerySocket(socket_object& so, uv_os_sock_t s) = 0;
		};

		template<typename _UVH>
		void _handle_comeback(_UVH* handle)
		{
			CHandle Handle(handle);
			// back to task
			SwitchToFiber(Handle.GetXTask()->GetFiber());
		}

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
				assert(GetCurrentFiber() != m_fiber);

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
				task->Run();
				IXScheduler* scheduler = task->GetXOwner();

				scheduler->FreeTask(task);
				SwitchToFiber(scheduler->GetFiber());
			}
		public:
			virtual FIBER_T GetFiber() const override { return m_fiber; }
			virtual IScheduler* GetOwner() const override { return m_owner; }
			virtual IXScheduler* GetXOwner() const override { return m_owner; }
		public:
			virtual void Sleep(std::uint64_t ms) override
			{
				CHandle sleep_handle(UV_TIMER, this);

				uv_timer_init(m_owner->GetLoopContext(), sleep_handle);
				uv_timer_start(sleep_handle, _handle_comeback, ms, 0);
				// switch to Scheduler
				SwitchToFiber(m_owner->GetFiber());
				// come back, oh yeah !!!
				sleep_handle.Close();
			}
		public: // socket
			virtual uv_os_sock_t socket(int af, int type, int protocol) override
			{
				throw std::logic_error("The method or operation is not implemented.");
			}
			virtual int bind(uv_os_sock_t s, const struct sockaddr* addr, int namelen) override
			{
				throw std::logic_error("The method or operation is not implemented.");
			}
			virtual int listen(uv_os_sock_t s, int backlog) override
			{
				throw std::logic_error("The method or operation is not implemented.");
			}
			virtual uv_os_sock_t accept(uv_os_sock_t s, struct sockaddr* addr, int* addrlen) override
			{
				throw std::logic_error("The method or operation is not implemented.");
			}
			virtual int connect(uv_os_sock_t s, const struct sockaddr* name, int namelen) override
			{
				throw std::logic_error("The method or operation is not implemented.");
			}
			virtual int send(uv_os_sock_t s, const char* buf, int len, int flags) override
			{
				throw std::logic_error("The method or operation is not implemented.");
			}
			virtual int recv(uv_os_sock_t s, char* buf, int len, int flags) override
			{
				throw std::logic_error("The method or operation is not implemented.");
			}
			virtual int shutdown(uv_os_sock_t s, int how) override
			{
				throw std::logic_error("The method or operation is not implemented.");
			}
			virtual int closesocket(uv_os_sock_t s) override
			{
				throw std::logic_error("The method or operation is not implemented.");
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
				assert(m_fiber == GetCurrentFiber());

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
					CHandle Handle(UV_TIMER, task);

					if ((uv_timer_init(m_loop_context, Handle) == 0)
						&& (uv_timer_start(Handle, _task_execute, 0, 0) == 0))
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
				int errcode;
				CHandle Handle(UV_TIMER, task);

				errcode = uv_timer_init(m_loop_context, Handle);
				assert(errcode == 0);
				errcode = uv_timer_start(Handle, _task_finished, 0, 0);
				assert(errcode == 0);
			}
		protected:
			static void _task_execute(uv_timer_t* handle)
			{
				CHandle Handle(handle);
				IXTask* task = Handle.GetXTask();

				Handle.Close();
				SwitchToFiber(task->GetFiber());
			}
			static void _task_finished(uv_timer_t* handle)
			{
				CHandle Handle(handle);
				IXTask* task = Handle.GetXTask();

				Handle.Close();
				task->Delete();
			}
		public: // socket
			virtual bool NewSocket(socket_object& so, int af, int type, int protocol) override
			{
				return false;
			}
			virtual bool FreeSocket(uv_os_sock_t s) override
			{
				auto iter = m_socket_table.find(s);

				if (iter != m_socket_table.end())
				{
					socket_object& so = iter->second;

					so.handle.Close();
					m_socket_table.erase(iter);
					return true;
				}
				return false;
			}
			virtual bool QuerySocket(socket_object& so, uv_os_sock_t s) override
			{
				auto iter = m_socket_table.find(s);

				if (iter != m_socket_table.end())
				{
					so = iter->second;
					return true;
				}
				return false;
			}
		private:
			FIBER_T m_fiber;
			bool m_was_converted;
			uv_loop_t* m_loop_context;
			std::unordered_map<uv_os_sock_t, socket_object> m_socket_table;
		};
	} // namespace impl

	IScheduler* CreateScheduler() { return impl::CXScheduler::Create(); }
}

