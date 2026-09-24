/*
 * Copyright (c) 2016 The ZLToolKit project authors. All Rights Reserved.
 *
 * This file is part of ZLToolKit(https://github.com/ZLMediaKit/ZLToolKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

//回归:EventPoller的最后一个引用在它自己的轮询线程上、且是在延时任务里释放时,
//对象必须仍能被销毁(销毁被推迟到runLoop返回之后,并靠管道唤醒循环)。
//通过捕获~EventPoller打出的那条日志来判断销毁是否发生,以便在各平台一致地检测。
//Regression: when the last reference to an EventPoller goes away on its own polling thread from
//inside a delayed task, the object still has to be destroyed (destruction is deferred until runLoop
//has returned and the loop is woken through the pipe). The log line printed by ~EventPoller is
//captured to tell whether destruction happened, so the check works the same on every platform.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "Poller/EventPoller.h"
#include "Util/logger.h"

using namespace toolkit;

static std::atomic<bool> g_dtor_seen { false };

class CaptureChannel : public LogChannel {
public:
    CaptureChannel() : LogChannel("capture", LTrace) {}
    void write(const Logger &, const LogContextPtr &ctx) override {
        if (ctx->_function.find("~EventPoller") != std::string::npos) {
            g_dtor_seen = true;
        }
    }
};

struct Pool : public TaskExecutorGetterImp {
    Pool() { addPoller("deferred", 1, ThreadPool::PRIORITY_NORMAL, false, false); }
    EventPoller::Ptr poller() { return std::static_pointer_cast<EventPoller>(_threads[0]); }
};

int main() {
    Logger::Instance().add(std::make_shared<CaptureChannel>());
    auto pool = new Pool();
    auto poller = pool->poller();
    //延时任务的闭包持有poller;任务执行完被销毁时,它释放的就是最后一个引用,且发生在轮询线程上
    //The closure of the delayed task holds the poller; when the task is destroyed after running, it
    //drops the last reference, on the polling thread
    poller->doDelayTask(50, [poller]() { return 0; });
    poller.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    delete pool;
    for (int i = 0; i < 100 && !g_dtor_seen; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("deferred delete: poller %s\n", g_dtor_seen ? "destroyed" : "NOT destroyed within 2s");
    return g_dtor_seen ? 0 : 1;
}
