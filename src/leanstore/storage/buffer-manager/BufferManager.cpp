#include "BufferManager.hpp"
#include "BufferFrame.hpp"
#include "AsyncWriteBuffer.hpp"
#include "leanstore/random-generator/RandomGenerator.hpp"
#include "leanstore/storage/btree/BTreeOptimistic.hpp"
// -------------------------------------------------------------------------------------
#include <gflags/gflags.h>
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_OFF
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/rotating_file_sink.h" // support for rotating file logging
// -------------------------------------------------------------------------------------
#include <fcntl.h>
#include <unistd.h>
#include <emmintrin.h>
// -------------------------------------------------------------------------------------
DEFINE_uint32(dram_pages, 10 * 1000, "");
DEFINE_uint32(ssd_pages, 100 * 1000, "");
DEFINE_string(ssd_path, "leanstore", "");
DEFINE_bool(ssd_truncate, true, "");
// -------------------------------------------------------------------------------------
DEFINE_uint32(cooling_threshold, 90, "Start cooling pages when 100-x% are free");
// -------------------------------------------------------------------------------------
DEFINE_uint32(background_write_sleep, 10, "us");
DEFINE_uint32(write_buffer_size, 100, "");
DEFINE_uint32(async_batch_size, 10, "");
// -------------------------------------------------------------------------------------
namespace leanstore {
BufferManager::BufferManager()
{
   // -------------------------------------------------------------------------------------
   // Init DRAM pool
   {
      const u64 dram_total_size = sizeof(BufferFrame) * u64(FLAGS_dram_pages);
      bfs = reinterpret_cast<BufferFrame *>(mmap(NULL, dram_total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
      madvise(bfs, dram_total_size, MADV_HUGEPAGE);
      memset(bfs, 0, dram_total_size);
      dram_free_bfs_counter = FLAGS_dram_pages;
   }
   // -------------------------------------------------------------------------------------
   // Init SSD pool
   const u32 ssd_total_size = FLAGS_ssd_pages * PAGE_SIZE;
   int flags = O_RDWR | O_DIRECT | O_CREAT;
   if ( FLAGS_ssd_truncate ) {
      flags |= O_TRUNC;
   }
   ssd_fd = open(FLAGS_ssd_path.c_str(), flags, 0666);
   check(ssd_fd > -1);
   check(ftruncate(ssd_fd, ssd_total_size) == 0);
   if ( fcntl(ssd_fd, F_GETFL) == -1 ) {
      throw Generic_Exception("Can not initialize SSD storage: " + FLAGS_ssd_path);
   }
   // -------------------------------------------------------------------------------------
   for ( u64 bf_i = 0; bf_i < FLAGS_dram_pages; bf_i++ ) {
      dram_free_bfs.push(new(bfs + bf_i) BufferFrame());
   }
   for ( u64 pid = 0; pid < FLAGS_ssd_pages; pid++ ) {
      cooling_io_ht.emplace(std::piecewise_construct, std::forward_as_tuple(pid), std::forward_as_tuple());
      ssd_free_pages.push(pid);
   }
   // -------------------------------------------------------------------------------------
   // Background threads
   std::thread page_provider_thread([&]() {
      pthread_setname_np(pthread_self(), "page_provider");
      auto logger = spdlog::rotating_logger_mt("PageProviderThread", "page_provider.txt", 1024 * 1024, 1);
      // -------------------------------------------------------------------------------------
      // Init AIO Context
      // TODO: own variable for page provider write buffer size
      AsyncWriteBuffer async_write_buffer(PAGE_SIZE, FLAGS_write_buffer_size, ssd_fd);
      // -------------------------------------------------------------------------------------
      BufferFrame *r_buffer = &randomBufferFrame();
      bool to_cooling_stage = 1;
      while ( bg_threads_keep_running ) {
         try {
            if ( to_cooling_stage ) {
               // unswizzle pages (put in the cooling stage)
               if ( dram_free_bfs_counter * 100.0 / FLAGS_dram_pages <= FLAGS_cooling_threshold ) {
                  SharedGuard r_guard(r_buffer->header.lock);
                  const bool is_cooling_candidate = r_buffer->header.state == BufferFrame::State::HOT; // && !rand_buffer->header.isWB
                  if ( !is_cooling_candidate ) {
                     r_buffer = &randomBufferFrame();
                     continue;
                  }
                  r_guard.recheck();
                  // -------------------------------------------------------------------------------------
                  bool picked_a_child_instead = false;
                  dt_registry.iterateChildrenSwips(r_buffer->page.dt_id,
                                                   *r_buffer, r_guard, [&](Swip<BufferFrame> &swip) {
                             if ( swip.isSwizzled()) {
                                picked_a_child_instead = true;
                                r_buffer = &swip.asBufferFrame();
                                r_guard.recheck();
                                return false;
                             }
                             r_guard.recheck();
                             return true;
                          });
                  if ( picked_a_child_instead ) {
                     logger->info("picked a child instead");
                     continue; //restart the inner loop
                  }
                  // -------------------------------------------------------------------------------------
                  {
                     ExclusiveGuard r_x_guad(r_guard);
                     ParentSwipHandler parent_handler = dt_registry.findParent(r_buffer->page.dt_id, *r_buffer, r_guard);
                     ExclusiveGuard p_x_guard(parent_handler.guard);
                     std::lock_guard g_guard(global_mutex); // must accquire the mutex before exclusive locks
                     assert(parent_handler.guard.local_version == parent_handler.guard.version_ptr->load());
                     assert(parent_handler.swip.bf == r_buffer);
                     logger->info("PID {} gonna get cool, type = {}", r_buffer->header.pid, u8(reinterpret_cast<btree::NodeBase *>(r_buffer->page.dt)->type));
                     parent_handler.swip.unswizzle(r_buffer->header.pid);
                     CIOFrame &cio_frame = cooling_io_ht[r_buffer->header.pid];
                     cio_frame.state = CIOFrame::State::COOLING;
                     cooling_fifo_queue.push_back(r_buffer);
                     cio_frame.fifo_itr = --cooling_fifo_queue.end();
                     r_buffer->header.state = BufferFrame::State::COLD;
                     // -------------------------------------------------------------------------------------
                     stats.unswizzled_pages_counter++;
                  }
                  r_buffer = &randomBufferFrame();
                  to_cooling_stage = !to_cooling_stage;
               }
            } else { // out of the cooling stage
               // AsyncWrite (for dirty) or remove (clean) the oldest (n) pages from fifo
               std::unique_lock g_guard(global_mutex);
               std::lock_guard reservoir_guard(reservoir_mutex);
               //TODO: other variable than async_batch_size
               u64 n_to_process_bfs = std::min(cooling_fifo_queue.size(), size_t(FLAGS_async_batch_size));
               auto bf_itr = cooling_fifo_queue.begin();
               while ( bf_itr != cooling_fifo_queue.end() ) {
                  BufferFrame &bf = **bf_itr;
                  auto next_bf_tr = bf_itr.e;
                  // TODO: can we write multiple  versions sim ?
                  // TODO: current implemetnation assume that checkpoint thread does not touch the
                  // the cooled pages
                  if ( bf.header.isWB == false ) {
                     if ( !bf.isDirty()) {
                        // Reclaim buffer frame
                        cooling_fifo_queue.erase(bf_itr++);
                        cooling_io_ht[bf.header.pid].state = CIOFrame::State::NOT_LOADED;
                        dram_free_bfs.push(&bf);
                        dram_free_bfs_counter++;
                     } else {
                        //TODO: optimize this path: an array for shared/ex guards and writeasync out of the global lock
                        async_write_buffer.add(bf);
                     }
                  }
                  bf_itr = next_bf_tr;
               }
               g_guard.unlock();
               async_write_buffer.submitIfNecessary([&](BufferFrame &written_bf, u64 written_lsn) {
                  while ( true ) {
                     try {
                        SharedGuard guard(written_bf.header.lock);
                        ExclusiveGuard x_guard(guard);
                        written_bf.header.lastWrittenLSN = written_lsn;
                        written_bf.header.isWB = false;
                     } catch ( RestartException e ) {
                     }
                  }
               }, FLAGS_async_batch_size); // TODO: own gflag for batch size
               to_cooling_stage = !to_cooling_stage;
            }
         } catch ( RestartException e ) {
         }
      }
      bg_threads_counter--;
      logger->info("end");
   });
   bg_threads_counter++;
   page_provider_thread.detach();
   // -------------------------------------------------------------------------------------
   //
   std::thread checkpoint_thread([&]() {
      pthread_setname_np(pthread_self(), "checkpoint");
      auto logger = spdlog::rotating_logger_mt("CheckPointThread", "checkpoint_thread.txt", 1024 * 1024, 1);
      // -------------------------------------------------------------------------------------
      // Init AIO stack
      AsyncWriteBuffer async_write_buffer(PAGE_SIZE, FLAGS_write_buffer_size, ssd_fd);
      // -------------------------------------------------------------------------------------
      while ( false && bg_threads_keep_running ) {
         try {
            BufferFrame &rand_buffer = randomBufferFrame();
            SharedGuard lock(rand_buffer.header.lock);
            const bool is_checkpoint_candidate = rand_buffer.header.state != BufferFrame::State::FREE
                                                 && !rand_buffer.header.isWB
                                                 && rand_buffer.header.lastWrittenLSN != rand_buffer.page.LSN;
            if ( is_checkpoint_candidate ) {
               ExclusiveGuard x_lock(lock);
               logger->info("found candidate for checkpoint {} - {}", rand_buffer.header.lastWrittenLSN, rand_buffer.page.LSN);
               async_write_buffer.add(rand_buffer);
            }
         } catch ( RestartException e ) {
         }
         async_write_buffer.submitIfNecessary([](BufferFrame &written_bf, u64 written_LSN) {
            while ( true ) {
               try {
                  SharedGuard guard(written_bf.header.lock);
                  ExclusiveGuard x_guard(guard);
                  written_bf.header.lastWrittenLSN = written_LSN;
                  written_bf.header.isWB = false;
               } catch ( RestartException e ) {
               }
            }
         }, FLAGS_async_batch_size);
      }
      bg_threads_counter--;
      logger->info("end");
   });
//   bg_threads_counter++;
   checkpoint_thread.detach();
   // -------------------------------------------------------------------------------------
}
// -------------------------------------------------------------------------------------
// Buffer Frames Management
// -------------------------------------------------------------------------------------
BufferFrame &BufferManager::randomBufferFrame()
{
   auto rand_buffer_i = RandomGenerator::getRand<u64>(0, FLAGS_dram_pages);
   return bfs[rand_buffer_i];
}
// -------------------------------------------------------------------------------------
// returns a *write locked* new buffer frame
BufferFrame &BufferManager::allocatePage()
{
   std::lock_guard lock(reservoir_mutex);
   assert(ssd_free_pages.size());
   assert(dram_free_bfs.size());
   auto free_pid = ssd_free_pages.front();
   ssd_free_pages.pop();
   auto free_bf = dram_free_bfs.front();
   // -------------------------------------------------------------------------------------
   // Initialize Buffer Frame
   free_bf->header.pid = free_pid;
   free_bf->header.lock = 2; // Write lock
   free_bf->header.state = BufferFrame::State::HOT;
   free_bf->header.lastWrittenLSN = free_bf->page.LSN;
   // -------------------------------------------------------------------------------------
   dram_free_bfs.pop();
   dram_free_bfs_counter--;
   // -------------------------------------------------------------------------------------
   return *free_bf;
}
// -------------------------------------------------------------------------------------
BufferFrame &BufferManager::resolveSwip(SharedGuard &swip_guard, Swip<BufferFrame> &swip_value) // throws RestartException
{
   static auto logger = spdlog::rotating_logger_mt("ResolveSwip", "resolve_swip.txt", 1024 * 1024, 1);
   if ( swip_value.isSwizzled()) {
      BufferFrame &bf = swip_value.asBufferFrame();
      swip_guard.recheck();
      return bf;
   }
   logger->info("WorkerThread: checking the CIOTable for pid {}", swip_value.asPageID());
   std::unique_lock g_guard(global_mutex);
   swip_guard.recheck();
   assert(!swip_value.isSwizzled());
   CIOFrame &cio_frame = cooling_io_ht.find(swip_value.asPageID())->second;
   if ( cio_frame.state == CIOFrame::State::NOT_LOADED ) {
      logger->info("WorkerThread::resolveSwip:not loaded state");
      cio_frame.readers_counter++;
      cio_frame.state = CIOFrame::State::READING;
      cio_frame.mutex.lock();
      // -------------------------------------------------------------------------------------
      reservoir_mutex.lock(); //TODO: deadlock ?
      g_guard.unlock();
      assert(dram_free_bfs.size());
      BufferFrame &bf = *dram_free_bfs.front();
      dram_free_bfs.pop();
      reservoir_mutex.unlock();
      // -------------------------------------------------------------------------------------
      readPageSync(swip_value.asPageID(), bf.page);
      bf.header.lastWrittenLSN = bf.page.LSN;
      // -------------------------------------------------------------------------------------
      // Move to cooling stage
      g_guard.lock();
      cio_frame.state = CIOFrame::State::COOLING;
      cooling_fifo_queue.push_back(&bf);
      cio_frame.fifo_itr = --cooling_fifo_queue.end();
      g_guard.unlock();
      cio_frame.mutex.unlock();
      throw RestartException();
      // TODO: do we really need to clean up ?
   }
   if ( cio_frame.state == CIOFrame::State::READING ) {
      logger->info("WorkerThread::resolveSwip:Reading state");
      cio_frame.readers_counter++;
      g_guard.unlock();
      cio_frame.mutex.lock();
      cio_frame.readers_counter--;
      cio_frame.mutex.unlock();
      throw RestartException();
   }
   /*
    * Lessons learned here:
    * don't catch a restart exception here
    * Whenever we fail to accquire a lock or witness a version change
    * then we have to read the value ! (update SharedGuard)
    * otherwise we would stick with the wrong version the whole time
    * and nasty things would happen
    */
   if ( cio_frame.state == CIOFrame::State::COOLING ) {
      logger->info("WorkerThread::resolveSwip:Cooling state");
      ExclusiveGuard x_lock(swip_guard);
      assert(!swip_value.isSwizzled());
      BufferFrame *bf = *cio_frame.fifo_itr;
      cooling_fifo_queue.erase(cio_frame.fifo_itr);
      cio_frame.state = CIOFrame::State::NOT_LOADED;
      bf->header.state = BufferFrame::State::HOT;
      // -------------------------------------------------------------------------------------
      swip_value.swizzle(bf);
      // -------------------------------------------------------------------------------------
      stats.swizzled_pages_counter++;
      logger->info("WorkerThread::resolveSwip:Cooling state - swizzled in");
      return *bf;
   }
   // it is a bug signal, if the page was hot then we should never hit this path
   UNREACHABLE();
}
// -------------------------------------------------------------------------------------
// SSD management
// -------------------------------------------------------------------------------------
void BufferManager::readPageSync(u64 pid, u8 *destination)
{
   assert(u64(destination) % 512 == 0);
   s64 read_bytes = pread(ssd_fd, destination, PAGE_SIZE, pid * PAGE_SIZE);
   check(read_bytes == PAGE_SIZE);
}
// -------------------------------------------------------------------------------------
void BufferManager::flush()
{
   fdatasync(ssd_fd);
}
// -------------------------------------------------------------------------------------
// Datastructures management
// -------------------------------------------------------------------------------------
void BufferManager::registerDatastructureType(leanstore::DTType type, leanstore::DTRegistry::CallbackFunctions callback_functions)
{
   dt_registry.callbacks_ht.insert({type, callback_functions});
}
// -------------------------------------------------------------------------------------
void BufferManager::registerDatastructureInstance(DTID dtid, leanstore::DTType type, void *root_object)
{
   dt_registry.dt_meta_ht.insert({dtid, {type, root_object}});
}
// -------------------------------------------------------------------------------------
unique_ptr<BufferManager> BMC::global_bf(nullptr);
void BMC::start()
{
   global_bf = make_unique<BufferManager>();
}
// -------------------------------------------------------------------------------------
void BufferManager::stopBackgroundThreads()
{
   bg_threads_keep_running = false;
   while ( bg_threads_counter ) {
      _mm_pause();
   }
}
// -------------------------------------------------------------------------------------
BufferManager::~BufferManager()
{
   stopBackgroundThreads();
   const u64 dram_total_size = sizeof(BufferFrame) * u64(FLAGS_dram_pages);
   close(ssd_fd);
   ssd_fd = -1;
   munmap(bfs, dram_total_size);
   // -------------------------------------------------------------------------------------
   cout << "Stats" << endl;
   cout << "swizzled counter = " << stats.swizzled_pages_counter << endl;
   cout << "unswizzled counter = " << stats.unswizzled_pages_counter << endl;
   // -------------------------------------------------------------------------------------
   // TODO: save states in YAML
}
// -------------------------------------------------------------------------------------
}
// -------------------------------------------------------------------------------------