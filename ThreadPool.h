#ifndef _THREAD_POOL_
#define _THREAD_POOL_
#include <queue>
#include <condition_variable>
#include <future>
#include <functional>
#include <thread>
#include <mutex>
#include <vector>
#include <stdexcept>

#if __cplusplus < 201402L
namespace std {
    template<class T> struct _Unique_if {
        typedef unique_ptr<T> _Single_object;
    };

    template<class T> struct _Unique_if<T[]> {
        typedef unique_ptr<T[]> _Unknown_bound;
    };

    template<class T, size_t N> struct _Unique_if<T[N]> {
        typedef void _Known_bound;
    };

    template<class T, class... Args>
        typename _Unique_if<T>::_Single_object
        make_unique(Args&&... args) {
            return unique_ptr<T>(new T(std::forward<Args>(args)...));
        }

    template<class T>
        typename _Unique_if<T>::_Unknown_bound
        make_unique(size_t n) {
            typedef typename remove_extent<T>::type U;
            return unique_ptr<T>(new U[n]());
        }

    template<class T, class... Args>
        typename _Unique_if<T>::_Known_bound
        make_unique(Args&&...) = delete;
}
#endif

using namespace std;
class ThreadPool {
public:
  ThreadPool(size_t);
  template<class F, class... Args>
  auto enqueue(F&& f, Args&&... args)
    ->std::future<typename std::result_of<F(Args...)>::type>;
  ~ThreadPool();
  void wait();
private:
  // need to keep track of threads so we can join them
  std::vector< std::thread > workers;
  // the task queue
  std::queue< std::function<void()> > tasks;

  // synchronization
  std::mutex queue_mutex;
  std::condition_variable condition;
  
  size_t tasks_left;
  std::mutex tasks_left_mutex;
  std::condition_variable wait_condition;

  bool stop;
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool(size_t threads)
  : stop(false), tasks_left(0)
{
  for (size_t i = 0; i<threads; ++i)
    workers.emplace_back(
      [this]
  {
    for (;;)
    {
      std::function<void()> task;
      // make unique lock vaild
      {
        std::unique_lock<std::mutex> lock(this->queue_mutex);
        this->condition.wait(lock,
          [this] { return this->stop || !this->tasks.empty(); });
        if (this->stop && this->tasks.empty())
          return;
        task = std::move(this->tasks.front());
        this->tasks.pop();
      }

      task();
      {
        std::lock_guard<std::mutex> lock(tasks_left_mutex);
        tasks_left --;
      }
      wait_condition.notify_one();
    }
  }
  );
}

// add new work item to the pool
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
-> std::future<typename std::result_of<F(Args...)>::type>
{
  using return_type = typename std::result_of<F(Args...)>::type;

  auto task = std::make_shared< std::packaged_task<return_type()> >(
    std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

  std::future<return_type> res = task->get_future();
  {
    std::lock_guard<std::mutex> lock(queue_mutex);

    // don't allow enqueueing after stopping the pool
    if (stop)
      throw std::runtime_error("enqueue on stopped ThreadPool");

    tasks.emplace([task]() { (*task)(); });
  }
  {
    std::lock_guard<std::mutex> lock(tasks_left_mutex);
    tasks_left ++;
  }
  condition.notify_one();
  return res;
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool()
{
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    stop = true;
  }
  condition.notify_all();
  for (std::thread &worker : workers)
    worker.join();
}

inline void ThreadPool::wait() {
  std::unique_lock<std::mutex> lock(tasks_left_mutex);
  if (tasks_left > 0) {
    wait_condition.wait(lock, [this] {return tasks_left == 0;});
  }
}

#endif
