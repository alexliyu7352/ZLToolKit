/*
 * Copyright (c) 2016 The ZLToolKit project authors. All Rights Reserved.
 *
 * This file is part of ZLToolKit(https://github.com/ZLMediaKit/ZLToolKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "Util/local_time.h"
#include "Util/util.h"

using namespace toolkit;

//本用例覆盖时区偏移的正确性，判定标准统一为"与libc的localtime结果完全一致"——
//这比写死期望值更强，也不必随时区数据库的更新而维护。
//时区一律使用POSIX TZ字符串而非IANA名称，因为Windows的_tzset只认前者。
//This case covers the correctness of the timezone offset. The criterion is always
//"identical to what libc's localtime returns", which is a stronger check than hardcoded
//expectations and needs no maintenance when the timezone database is updated.
//Timezones are given as POSIX TZ strings rather than IANA names, because the _tzset of
//Windows only understands the former.
static void useTimezone(const char *tz) {
#ifdef _WIN32
    _putenv_s("TZ", tz);
    _tzset();
#else
    setenv("TZ", tz, 1);
    tzset();
#endif
    //绕开最长60秒的校准间隔，让改动立刻生效
    //Bypass the calibration interval of up to 60 seconds so the change takes effect at once
    local_time_refresh();
}

static struct tm libcLocalTime(time_t sec) {
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &sec);
#else
    localtime_r(&sec, &tm);
#endif
    return tm;
}

//逐字段比较本库与libc的换算结果
//Compare the result of this library with libc field by field
static bool sameAsLibc(const char *tz) {
    useTimezone(tz);
    //只能取当前时刻: 夏令时状态是按"现在"取得并套用于所有时间戳的，
    //查询其它季节的时间戳本就会相差一小时，那是已知限制而非缺陷
    //Only the current moment can be used: the daylight saving state reflects "now" and is
    //applied to every timestamp, so querying another season is off by one hour by design
    time_t now = time(nullptr);
    auto got = getLocalTime(now);
    auto expect = libcLocalTime(now);

    if (got.tm_year != expect.tm_year || got.tm_mon != expect.tm_mon || got.tm_mday != expect.tm_mday
        || got.tm_hour != expect.tm_hour || got.tm_min != expect.tm_min || got.tm_sec != expect.tm_sec) {
        printf("[FAIL] TZ=%s 时刻不一致: 本库=%04d-%02d-%02d %02d:%02d:%02d libc=%04d-%02d-%02d %02d:%02d:%02d\n", tz,
               1900 + got.tm_year, 1 + got.tm_mon, got.tm_mday, got.tm_hour, got.tm_min, got.tm_sec,
               1900 + expect.tm_year, 1 + expect.tm_mon, expect.tm_mday, expect.tm_hour, expect.tm_min,
               expect.tm_sec);
        return false;
    }
#ifndef _WIN32
    //tm_gmtoff是glibc/BSD的扩展字段，Windows的struct tm没有该成员
    //tm_gmtoff is a glibc/BSD extension, the struct tm of Windows has no such member
    if (got.tm_gmtoff != expect.tm_gmtoff) {
        printf("[FAIL] TZ=%s 偏移不一致: 本库=%ld libc=%ld\n", tz, (long)got.tm_gmtoff, (long)expect.tm_gmtoff);
        return false;
    }
#endif

    //以%Z格式化会读取tm_zone。该字段一旦没有被填写，读到的就是调用方栈上的残留指针，
    //轻则输出乱码、重则直接崩溃，且不一定每次都发作，故此处与libc逐字比对加以固定
    //Formatting with %Z reads tm_zone. If that field is left unset, whatever pointer happens
    //to be on the caller's stack gets dereferenced: garbled output at best, a crash at worst,
    //and not necessarily on every run; comparing it against libc pins the behaviour down
    char got_str[64], expect_str[64];
    if (!strftime(got_str, sizeof(got_str), "%Y-%m-%d %H:%M:%S %z [%Z]", &got)
        || !strftime(expect_str, sizeof(expect_str), "%Y-%m-%d %H:%M:%S %z [%Z]", &expect)) {
        printf("[FAIL] TZ=%s strftime失败\n", tz);
        return false;
    }
    if (strcmp(got_str, expect_str) != 0) {
        printf("[FAIL] TZ=%s 格式化结果不一致: 本库=%s libc=%s\n", tz, got_str, expect_str);
        return false;
    }

    //getTimeStr()是对外接口，下游拿它格式化时区名的可能性最大，一并覆盖
    //getTimeStr() is the public interface and the most likely place for a downstream project
    //to format the timezone name, so it is covered as well
    if (getTimeStr("%Y-%m-%d %H:%M:%S %z [%Z]", now) != expect_str) {
        printf("[FAIL] TZ=%s getTimeStr()结果不一致: 本库=%s libc=%s\n", tz,
               getTimeStr("%Y-%m-%d %H:%M:%S %z [%Z]", now).data(), expect_str);
        return false;
    }
    return true;
}

//换算出的时刻必须与同一结构体里的tm_gmtoff同源，
//否则会出现"时刻按夏令时算、偏移却按标准时标注"这种自相矛盾的输出
//The broken down time has to come from the same offset as the tm_gmtoff in that very
//structure, otherwise the output contradicts itself: the time following the daylight saving
//rule while the offset next to it states standard time
#ifndef _WIN32
static bool selfConsistent(const char *tz) {
    useTimezone(tz);
    time_t now = time(nullptr);
    auto tm = getLocalTime(now);
    time_t local = now + tm.tm_gmtoff;
    if (tm.tm_hour != (int)(local / 3600 % 24) || tm.tm_min != (int)(local / 60 % 60)
        || tm.tm_sec != (int)(local % 60)) {
        printf("[FAIL] TZ=%s 时刻与tm_gmtoff不同源: %02d:%02d:%02d vs 由偏移%ld反推的%02d:%02d:%02d\n", tz,
               tm.tm_hour, tm.tm_min, tm.tm_sec, (long)tm.tm_gmtoff, (int)(local / 3600 % 24),
               (int)(local / 60 % 60), (int)(local % 60));
        return false;
    }
    //getGMTOff()与getLocalTime()必须取自同一份数据，不能各自缓存一份
    //getGMTOff() and getLocalTime() must read the same data instead of caching one copy each
    if (getGMTOff() != tm.tm_gmtoff) {
        printf("[FAIL] TZ=%s getGMTOff()=%ld 与 tm_gmtoff=%ld 不一致\n", tz, getGMTOff(), (long)tm.tm_gmtoff);
        return false;
    }
    return true;
}
#endif

#ifndef _WIN32
//并发下tm_zone不得为空。这条用例针对的是一个具体的教训：曾经把tm_zone直接指向tzname[]，
//而glibc在每一次localtime_r()中都会改写该数组——先置NULL、算完再写回，全程持tzset_lock，
//本库却是不持锁读的，于是并发下读到NULL的比例可高达五成。单线程用例完全覆盖不到它。
//tm_zone must never be null under concurrency. This case guards a concrete lesson: tm_zone used
//to point straight into tzname[], which glibc rewrites on every localtime_r() call, setting it
//to NULL first and writing it back afterwards, all under tzset_lock while this library reads
//without any lock; the share of NULL readings reached fifty percent. A single threaded case
//cannot catch that at all.
static bool zoneStableUnderConcurrency() {
    //必须用读取时区数据库的时区名，不能用POSIX TZ字符串：glibc只有在走tzfile这条路径时
    //才会反复改写tzname[]，用"CST-8"之类的字符串跑，这条用例会静默地什么都测不到
    //A timezone name backed by the timezone database is required here, a POSIX TZ string will
    //not do: glibc only rewrites tzname[] on the tzfile code path, so running this case with
    //something like "CST-8" would silently exercise nothing
    useTimezone("Asia/Shanghai");

    std::atomic<bool> running { true };
    std::atomic<uint64_t> calls { 0 }, nulls { 0 };
    //持续触发localtime_r，模拟每60秒一次的校准、日志清理时的mktime以及宿主自身的时间调用
    //Keep triggering localtime_r to mimic the periodic calibration, the mktime of the log
    //cleanup and any time call the host program itself makes
    std::thread refresher([&]() {
        while (running) {
            local_time_refresh();
        }
    });
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            while (running) {
                auto tm = getLocalTime(time(nullptr));
                ++calls;
                if (!tm.tm_zone) {
                    ++nulls;
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    running = false;
    refresher.join();
    for (auto &t : readers) {
        t.join();
    }
    if (nulls) {
        printf("[FAIL] 并发下tm_zone读到空指针: %llu/%llu 次\n", (unsigned long long)nulls.load(),
               (unsigned long long)calls.load());
        return false;
    }
    return true;
}
#endif

int main() {
    //记录原有TZ，测试结束后恢复
    //Remember the original TZ and restore it when the test is over
    const char *old_tz = getenv("TZ");
    std::string saved_tz = old_tz ? old_tz : "";

    //覆盖整小时、半小时、三刻钟偏移，以及夏令时为一小时与半小时两种情形。
    //豪勋爵岛(LHST)是关键用例:它的夏令时只有半小时,按"夏令时=固定一小时"推算会错30分钟
    //Whole hour, half hour and three quarter offsets are covered, as well as daylight saving
    //times of one hour and of half an hour. Lord Howe Island (LHST) is the crucial case: its
    //daylight saving time is only half an hour, deriving it as a fixed hour is off by 30 minutes
    static const char *TIMEZONES[] = {
        "UTC0",                                              //零偏移
        "CST-8",                                             //整小时: UTC+8
        "EST5",                                              //整小时: UTC-5
        "IST-5:30",                                          //半小时: UTC+5:30
        "NPT-5:45",                                          //三刻钟: UTC+5:45
        "EST5EDT,M3.2.0,M11.1.0",                            //一小时夏令时
        "LHST-10:30LHDT-11,J1/00:00:00,J365/23:59:59",       //半小时夏令时(全年生效)
        "CHAST-12:45CHADT-13:45,J1/00:00:00,J365/23:59:59",  //三刻钟偏移叠加一小时夏令时
        "NUT11",                                             //UTC-11
        "LINT-14",                                           //UTC+14
    };

    int ret = 0;
    for (size_t i = 0; i < sizeof(TIMEZONES) / sizeof(TIMEZONES[0]); ++i) {
        if (!sameAsLibc(TIMEZONES[i])) {
            ret = 1;
            break;
        }
#ifndef _WIN32
        if (!selfConsistent(TIMEZONES[i])) {
            ret = 2;
            break;
        }
#endif
    }

#ifndef _WIN32
    if (ret == 0 && !zoneStableUnderConcurrency()) {
        ret = 3;
    }
#endif

    //恢复原有时区，避免影响同一进程内的后续代码
    //Restore the original timezone so that later code in the same process is unaffected
    if (saved_tz.empty()) {
#ifdef _WIN32
        _putenv_s("TZ", "");
#else
        unsetenv("TZ");
#endif
    } else {
        useTimezone(saved_tz.data());
    }

    if (ret == 0) {
        printf("local time regression passed\n");
    }
    return ret;
}
