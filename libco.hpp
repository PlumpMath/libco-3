#pragma once
#include <cassert>
#include <stdexcept>
#include <functional>

namespace libco
{
	class ITask;
	class IScheduler;
	using Routine = std::function<void(ITask*)>;

	class ITask
	{
	public:
		virtual IScheduler* GetOwner() const = 0;
	public:
		virtual void Sleep(std::uint64_t ms) = 0;
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
		using FIBER_T = LPVOID;

		class IXTask;
		class IXScheduler : public IScheduler
		{
		public:
			virtual void FreeTask(IXTask* task) = 0;
		public:
			virtual FIBER_T GetFiber() const = 0;
			virtual uv_loop_t* GetLoopContext() const = 0;
		};

		class IXTask : public ITask
		{
		public:
			virtual void Delete() = 0;
		public:
			virtual FIBER_T GetFiber() const = 0;
			virtual IXScheduler* GetXOwner() const = 0;
		};

		class CHandle
		{
		public:
			CHandle(IXTask* task, uv_handle_type h_type)
				: m_handle(Alloc(h_type))
			{
				assert(task != nullptr);
				m_handle->data = task;
			}
			CHandle(uv_handle_t* handle) : m_handle(handle)
			{
				assert(m_handle != nullptr);
			}
			CHandle(uv_timer_t* handle) : CHandle((uv_handle_t*)handle) { }
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
			void Close()
			{
				assert(m_handle != nullptr);

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
				assert(m_handle != nullptr);
				return (IXTask*)m_handle->data;
			}
		public:
			operator uv_handle_t*() const { return m_handle; }
			operator uv_timer_t*() const { return (uv_timer_t*)m_handle; }
		private:
			uv_handle_t* m_handle;
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
				CHandle sleep_handle(this, UV_TIMER);

				uv_timer_init(m_owner->GetLoopContext(), sleep_handle);
				uv_timer_start(sleep_handle, _handle_comeback, ms, 0);
				// switch to Scheduler
				SwitchToFiber(m_owner->GetFiber());
				// come back, oh yeah !!!
				sleep_handle.Close();
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
					CHandle Handle(task, UV_TIMER);

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
				CHandle Handle(task, UV_TIMER);

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
		private:
			FIBER_T m_fiber;
			bool m_was_converted;
			uv_loop_t* m_loop_context;
		};
	} // namespace impl

	IScheduler* CreateScheduler() { return impl::CXScheduler::Create(); }
}

