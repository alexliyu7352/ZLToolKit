/*
 * Copyright (c) 2016 The ZLToolKit project authors. All Rights Reserved.
 *
 * This file is part of ZLToolKit(https://github.com/ZLMediaKit/ZLToolKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <memory>
#include <atomic>
#include "TaskExecutor.h"
#include "Poller/EventPoller.h"
#include "Util/onceToken.h"
#include "Util/TimeTicker.h"

using namespace std;

namespace toolkit {

ThreadLoadCounter::ThreadLoadCounter(uint64_t max_size, uint64_t max_usec) {
    _last_sleep_time = _last_wake_time = getCurrentMicrosecond();
    _max_size = max_size;
    _max_usec = max_usec;
}

void ThreadLoadCounter::startSleep() {
    lock_guard<mutex> lck(_mtx);
    _sleeping = true;
    auto current_time = getCurrentMicrosecond();
    auto run_time = current_time - _last_wake_time;
    _last_sleep_time = current_time;
    _time_list.emplace_back(run_time, false);
    if (_time_list.size() > _max_size) {
        _time_list.pop_front();
    }
}

void ThreadLoadCounter::sleepWakeUp() {
    lock_guard<mutex> lck(_mtx);
    _sleeping = false;
    auto current_time = getCurrentMicrosecond();
    auto sleep_time = current_time - _last_sleep_time;
    _last_wake_time = current_time;
    _time_list.emplace_back(sleep_time, true);
    if (_time_list.size() > _max_size) {
        _time_list.pop_front();
    }
}

int ThreadLoadCounter::load() {
    lock_guard<mutex> lck(_mtx);
    uint64_t totalSleepTime = 0;
    uint64_t totalRunTime = 0;
    _time_list.for_each([&](const TimeRecord &rcd) {
        if (rcd._sleep) {
            totalSleepTime += rcd._time;
        } else {
            totalRunTime += rcd._time;
        }
    });

    if (_sleeping) {
        totalSleepTime += (getCurrentMicrosecond() - _last_sleep_time);
    } else {
        totalRunTime += (getCurrentMicrosecond() - _last_wake_time);
    }

    uint64_t totalTime = totalRunTime + totalSleepTime;
    while ((_time_list.size() != 0) && (totalTime > _max_usec || _time_list.size() > _max_size)) {
        TimeRecord &rcd = _time_list.front();
        if (rcd._sleep) {
            totalSleepTime -= rcd._time;
        } else {
            totalRunTime -= rcd._time;
        }
        totalTime -= rcd._time;
        _time_list.pop_front();
    }
    if (totalTime == 0) {
        return 0;
    }
    return (int) (totalRunTime * 100 / totalTime);
}

////////////////////////////////////////////////////////////////////////////

Task::Ptr TaskExecutorInterface::async_first(TaskIn task, bool may_sync) {
    return async(std::move(task), may_sync);
}

void TaskExecutorInterface::sync(const TaskIn &task) {
    semaphore sem;
    auto ret = async([&]() {
        onceToken token(nullptr, [&]() {
            //通过RAII原理防止抛异常导致不执行这句代码  [AUTO-TRANSLATED:206bd80e]
            //Prevent this code from not being executed due to an exception being thrown through RAII principle
            sem.post();
        });
        task();
    });
    if (ret && *ret) {
        sem.wait();
    }
}

void TaskExecutorInterface::sync_first(const TaskIn &task) {
    semaphore sem;
    auto ret = async_first([&]() {
        onceToken token(nullptr, [&]() {
            //通过RAII原理防止抛异常导致不执行这句代码  [AUTO-TRANSLATED:206bd80e]
            //Prevent this code from not being executed due to an exception being thrown through RAII principle
            sem.post();
        });
        task();
    });
    if (ret && *ret) {
        sem.wait();
    }
}

//////////////////////////////////////////////////////////////////

TaskExecutor::TaskExecutor(uint64_t max_size, uint64_t max_usec) : ThreadLoadCounter(max_size, max_usec) {}

//////////////////////////////////////////////////////////////////

TaskExecutor::Ptr TaskExecutorGetterImp::getExecutor() {
    auto thread_pos = _thread_pos;
    if (thread_pos >= _threads.size()) {
        thread_pos = 0;
    }

    TaskExecutor::Ptr executor_min_load = _threads[thread_pos];
    auto min_load = executor_min_load->load();

    for (size_t i = 0; i < _threads.size(); ++i) {
        ++thread_pos;
        if (thread_pos >= _threads.size()) {
            thread_pos = 0;
        }

        auto th = _threads[thread_pos];
        auto load = th->load();

        if (load < min_load) {
            min_load = load;
            executor_min_load = th;
        }
        if (min_load == 0) {
            break;
        }
    }
    _thread_pos = thread_pos;
    return executor_min_load;
}

vector<int> TaskExecutorGetterImp::getExecutorLoad() {
    vector<int> vec(_threads.size());
    int i = 0;
    for (auto &executor : _threads) {
        vec[i++] = executor->load();
    }
    return vec;
}

void TaskExecutorGetterImp::getExecutorDelay(const function<void(const vector<int> &)> &callback) {
    std::shared_ptr<vector<int> > delay_vec = std::make_shared<vector<int>>(_threads.size());
    shared_ptr<void> finished(nullptr, [callback, delay_vec](void *) {
        //此析构回调触发时，说明已执行完毕所有async任务  [AUTO-TRANSLATED:8adf8212]
        //When this destructor callback is triggered, it means all async tasks have been executed
        callback((*delay_vec));
    });
    int index = 0;
    for (auto &th : _threads) {
        std::shared_ptr<Ticker> delay_ticker = std::make_shared<Ticker>();
        th->async([finished, delay_vec, index, delay_ticker]() {
            (*delay_vec)[index] = (int) delay_ticker->elapsedTime();
        }, false);
        ++index;
    }
}

using onGetExecutor = std::function<void(const TaskExecutor::Ptr &)>;
class onGetExecutorCB {
public:
    onGetExecutorCB(onGetExecutor cb): _cb(std::move(cb)) {}

    void operator()(const TaskExecutor::Ptr &exe) {
        bool expected = false;
        if (_done.compare_exchange_strong(expected, true)) {
            _cb(exe);
            _cb = nullptr;
        }
    }

private:
    std::atomic<bool> _done { false };
    std::function<void(const TaskExecutor::Ptr &)> _cb;
};

void TaskExecutorGetterImp::getExecutor(const onGetExecutor &cb) {
    auto callback = std::make_shared<onGetExecutorCB>(cb);
    auto thread_pos = _thread_pos;
    if (thread_pos >= _threads.size()) {
        thread_pos = 0;
    }
    for (size_t i = 0; i < _threads.size(); ++i) {
        ++thread_pos;
        if (thread_pos >= _threads.size()) {
            thread_pos = 0;
        }
        auto &th = _threads[thread_pos];
        th->async([th, callback]() mutable { (*callback)(th); }, false);
    }
    _thread_pos = thread_pos;
}

void TaskExecutorGetterImp::for_each(const function<void(const TaskExecutor::Ptr &)> &cb) {
    for (auto &th : _threads) {
        cb(th);
    }
}

size_t TaskExecutorGetterImp::getExecutorSize() const {
    return _threads.size();
}

TaskExecutorGetterImp::~TaskExecutorGetterImp() {
    //先在本线程逐个停掉轮询线程,再释放引用。否则队列里残留的任务(例如Socket析构)会在轮询
    //线程上执行,并在那里释放掉poller的最后一个引用,对象于是死在自己的线程里,任务返回到
    //runLoop的循环条件时便读到了已释放的内存。
    //放在本基类而不是某个具体的池子里,是因为_threads归本类所有:EventPollerPool与
    //WorkThreadPool都经addPoller()把EventPoller放进来,两者需要的是同一份收尾逻辑
    //Stop each polling thread from this thread before releasing the references. Otherwise a task
    //left in the queue (the destruction of a Socket for instance) runs on the polling thread and
    //drops the last reference to the poller there, so the object dies on its own thread and the
    //task returns into a loop condition of runLoop that reads freed memory.
    //This belongs to the base class rather than to one particular pool because _threads is owned
    //here: both EventPollerPool and WorkThreadPool fill it with EventPoller through addPoller(),
    //so both need the very same teardown
    for (auto &th : _threads) {
        static_pointer_cast<EventPoller>(th)->shutdown();
    }
}

size_t TaskExecutorGetterImp::addPoller(const string &name, size_t size, int priority, bool register_thread, bool enable_cpu_affinity) {
    auto cpus = thread::hardware_concurrency();
    size = size > 0 ? size : cpus;
    for (size_t i = 0; i < size; ++i) {
        auto full_name = name + " " + to_string(i);
        auto cpu_index = i % cpus;
        EventPoller::Ptr poller(new EventPoller(full_name));
        poller->runLoop(false, register_thread);
        poller->async([cpu_index, full_name, priority, enable_cpu_affinity]() {
            // 设置线程优先级  [AUTO-TRANSLATED:2966f860]
            //Set thread priority
            ThreadPool::setPriority((ThreadPool::Priority)priority);
            // 设置线程名  [AUTO-TRANSLATED:f5eb4704]
            //Set thread name
            setThreadName(full_name.data());
            // 设置cpu亲和性  [AUTO-TRANSLATED:ba213aed]
            //Set CPU affinity
            if (enable_cpu_affinity) {
                setThreadAffinity(cpu_index);
            }
        });
        _threads.emplace_back(std::move(poller));
    }
    return size;
}

}//toolkit
