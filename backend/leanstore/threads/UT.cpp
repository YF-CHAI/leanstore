#include "UT.hpp"
namespace leanstore
{
namespace threads
{
constexpr u64 STACK_SIZE = 16 * 1024 * 1024;
std::atomic<bool> UserThreadManager::keep_running = false;
std::atomic<u64> UserThreadManager::running_threads = 0;
std::mutex UserThreadManager::utm_mutex;
std::vector<std::thread> UserThreadManager::worker_threads;
std::vector<UserThread> UserThreadManager::uts;
std::vector<u64> UserThreadManager::uts_ready;
std::vector<u64> UserThreadManager::uts_blocked;
static thread_local s64 current_user_thread_slot = -1;
static thread_local ucontext_t* current_uctx = nullptr;
static thread_local s64 worker_id = -1;
// -------------------------------------------------------------------------------------
static void exec()
{
  assert(current_user_thread_slot != -1);
  UserThreadManager::uts[current_user_thread_slot].run();
}
// -------------------------------------------------------------------------------------
void UserThreadManager::destroy()
{
  while (uts_ready.size() > 0) {
  }
  keep_running = false;
  while (running_threads > 0) {
  }
  for (auto& wt : worker_threads) {
    wt.join();
  }
}
// -------------------------------------------------------------------------------------
void UserThreadManager::init(u64 n)
{
  keep_running = true;
  uts.reserve(1024);
  uts_ready.reserve(1024);
  for (u64 t_i = 0; t_i < n; t_i++)
    worker_threads.emplace_back([&, t_i]() {
      worker_id = t_i;
      running_threads++;
      ucontext_t worker_thread_uctx;
      current_uctx = &worker_thread_uctx;
      while (keep_running) {
        UserThread* th = nullptr;
        utm_mutex.lock();
        if (uts_ready.size() > 0) {
          current_user_thread_slot = uts_ready.back();
          th = &uts[uts_ready.back()];
          if (th->worker_id != -1 && th->worker_id != worker_id) {
            utm_mutex.unlock();
            continue;
          }
          uts_ready.pop_back();
        } else {
          current_user_thread_slot = -1;
        }
        utm_mutex.unlock();
        // -------------------------------------------------------------------------------------
        if (th != nullptr) {
          if (th->init == false) {
            th->context.uc_link = &worker_thread_uctx;
            makecontext(&th->context, (void (*)())exec, 0);
            th->init = true;
            th->worker_id = worker_id;
          }
          assert(current_user_thread_slot != -1);
          posix_check(swapcontext(current_uctx, &th->context) != -1);
        }
      }
      running_threads--;
    });
}
// -------------------------------------------------------------------------------------
void UserThreadManager::addThread(std::function<void()> run)
{
  utm_mutex.lock();
  uts.push_back({});
  auto& th = uts.back();
  th.run = run;
  th.init = false;
  posix_check(getcontext(&th.context) != -1);
  th.stack = make_unique<u8[]>(STACK_SIZE);
  th.context.uc_stack.ss_sp = th.stack.get();
  th.context.uc_stack.ss_size = STACK_SIZE;
  th.context.uc_link = nullptr;
  uts_ready.push_back(uts.size() - 1);
  utm_mutex.unlock();
}
// -------------------------------------------------------------------------------------
void UserThreadManager::asyncCall(std::function<void(std::function<void()>)> work)
{
  assert(current_user_thread_slot != -1);
  s64 slot_id = current_user_thread_slot;
  auto revive = [&, slot_id]() {
    utm_mutex.lock();
    uts_ready.push_back(slot_id);
    utm_mutex.unlock();
  };
  work(revive);
  posix_check(swapcontext(&uts[slot_id].context, current_uctx) != -1);
}
// -------------------------------------------------------------------------------------
}  // namespace threads
}  // namespace leanstore
