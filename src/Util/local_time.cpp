//
// Created by alex on 2022/5/29.
//

/*
 * Copyright (c) 2018, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <atomic>
#include <cstring>
#include <ctime>

#include "local_time.h"

/* This is a safe version of localtime() which contains no locks and is
 * fork() friendly. Even the _r version of localtime() cannot be used safely
 * in Redis. Another thread may be calling localtime() while the main thread
 * forks(). Later when the child process calls localtime() again, for instance
 * in order to log something to the Redis log, it may deadlock: in the copy
 * of the address space of the forked process the lock will never be released.
 *
 * 本实现与Redis原版的区别: 偏移量直接取自localtime()填充的tm_gmtoff，而不是
 * 由'timezone'全局变量加上固定1小时的夏令时修正推算，因此半小时夏令时的地区
 * (如澳大利亚豪勋爵岛)也能得到正确结果，且不依赖各平台对'timezone'的实现差异。
 * 该偏移由local_time_refresh()刷新，长时间运行的进程需要定期调用它，
 * 否则夏令时切换后本地时间会一直偏差。
 * Unlike the original Redis version, the offset is taken straight from the
 * tm_gmtoff filled in by localtime(), instead of being derived from the
 * 'timezone' global plus a hardcoded one hour daylight saving correction. Regions
 * with a half hour daylight saving time (Lord Howe Island for instance) are
 * therefore correct as well, and no assumption is made about how each platform
 * implements 'timezone'. The offset is refreshed by local_time_refresh(), which a
 * long running process has to call periodically, otherwise the local time would
 * stay off after a daylight saving time switch.
 *
 * Note that this function does not work for dates < 1/1/1970, it is solely
 * designed to work with what time(NULL) may return, and to support Redis
 * logging of the dates, it's not really a complete implementation. */
namespace toolkit {
/* 两者都由local_time_refresh()更新、被任意线程读取，故使用原子变量
 * Both are updated by local_time_refresh() and read by any thread, hence atomic. */
static std::atomic<int> _daylight_active { 0 };
/* 本地时间相对UTC的偏移(秒)，取自localtime()的tm_gmtoff，已含夏令时修正
 * Offset of the local time from UTC in seconds, taken from the tm_gmtoff of
 * localtime(), daylight saving time included */
static std::atomic<long> _local_gmtoff { 0 };
/* 时区缩写(如CST、EDT)。保存的是内容而非localtime()给出的指针：各家libc对该指针所指
 * 内存的生命周期约定不同，musl上TZ一变就会munmap掉(保存指针会悬垂、解引用即崩溃，已实测)。
 * 两块缓冲轮换：写非当前的那块，再发布索引，读者因而总能看到完整的一份。
 * The timezone abbreviation (CST, EDT and so on). The content is kept rather than the pointer
 * localtime() hands out: libcs differ on the lifetime of the memory it refers to, and on musl a
 * change of TZ munmaps it, so a saved pointer dangles and crashes on dereference (measured).
 * Two buffers take turns: the one not in use is written and the index published afterwards, so a
 * reader always sees a complete copy. */
static char _zone_name[2][16] = { "UTC", "UTC" };
static std::atomic<int> _zone_index { 0 };

int get_daylight_active() {
    return _daylight_active.load(std::memory_order_relaxed);
}

long get_local_gmtoff() {
    return _local_gmtoff.load(std::memory_order_relaxed);
}

static int is_leap_year(time_t year) {
    if (year % 4)
        return 0; /* A year not divisible by 4 is not leap. */
    else if (year % 100)
        return 1; /* If div by 4 and not 100 is surely leap. */
    else if (year % 400)
        return 0; /* If div by 100 *and* not by 400 is not leap. */
    else
        return 1; /* If div by 100 and 400 is leap. */
}

void no_locks_localtime(struct tm *tmp, time_t t) {
    const time_t secs_min = 60;
    const time_t secs_hour = 3600;
    const time_t secs_day = 3600 * 24;

    /* 偏移量只取一次快照，使换算出的时刻与tm_gmtoff必定同源；tm_isdst与tm_zone是另外两个
     * 独立原子量，切换那一瞬三者未必互洽(时刻本身仍是对的)。
     * A single snapshot of the offset keeps the broken down time and tm_gmtoff from the same
     * source; tm_isdst and tm_zone are two further independent atomics and the three need not
     * agree at the very instant of a switch, though the time itself stays correct. */
    int daylight_active = get_daylight_active();
    long gmtoff = get_local_gmtoff();

    t += gmtoff; /* Adjust for timezone and daylight time. */
    time_t days = t / secs_day; /* Days passed since epoch. */
    time_t seconds = t % secs_day; /* Remaining seconds. */

    tmp->tm_isdst = daylight_active;
    tmp->tm_hour = seconds / secs_hour;
    tmp->tm_min = (seconds % secs_hour) / secs_min;
    tmp->tm_sec = (seconds % secs_hour) % secs_min;
#ifndef _WIN32
    tmp->tm_gmtoff = gmtoff;
    /* tm_zone不填的话就是调用方栈上的未初始化指针，一旦用%Z格式化便会读到非法内存
     * Leaving tm_zone alone would keep whatever uninitialized pointer the caller has on its
     * stack, and formatting with %Z would then read invalid memory */
    tmp->tm_zone = _zone_name[_zone_index.load(std::memory_order_acquire)];
#endif
    /* 1/1/1970 was a Thursday, that is, day 4 from the POV of the tm structure
     * where sunday = 0, so to calculate the day of the week we have to add 4
     * and take the modulo by 7. */
    tmp->tm_wday = (days + 4) % 7;

    /* Calculate the current year. */
    tmp->tm_year = 1970;
    while (1) {
        /* Leap years have one day more. */
        time_t days_this_year = 365 + is_leap_year(tmp->tm_year);
        if (days_this_year > days)
            break;
        days -= days_this_year;
        tmp->tm_year++;
    }
    tmp->tm_yday = days; /* Number of day of the current year. */

    /* We need to calculate in which month and day of the month we are. To do
     * so we need to skip days according to how many days there are in each
     * month, and adjust for the leap year that has one more day in February. */
    int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    mdays[1] += is_leap_year(tmp->tm_year);

    tmp->tm_mon = 0;
    while (days >= mdays[tmp->tm_mon]) {
        days -= mdays[tmp->tm_mon];
        tmp->tm_mon++;
    }

    tmp->tm_mday = days + 1; /* Add 1 since our 'days' is zero-based. */
    tmp->tm_year -= 1900; /* Surprisingly tm_year is year-1900. */
}

void local_time_init() {
    /* 偏移量由local_time_refresh()从localtime()取得，此处只需保证TZ已被解析
     * The offset is taken from localtime() by local_time_refresh(), so all that is
     * needed here is to make sure TZ has been parsed */
    tzset();
    local_time_refresh();
}

void local_time_refresh() {
    /* 夏令时可能在进程运行期间切换，故该标志必须定期刷新，
     * 否则本地时间会一直相差1小时，直到进程重启
     * The daylight saving time may switch while the process is running, so this
     * flag must be refreshed periodically, otherwise the local time would stay
     * one hour off until the process is restarted. */
    time_t t = time(NULL);
    struct tm aux;
#ifdef _WIN32
    localtime_s(&aux, &t);
    /* _mkgmtime会就地改写整个struct tm并把tm_isdst置0，故先取出来
     * _mkgmtime rewrites the whole struct tm in place and resets tm_isdst, so read it first */
    int isdst = aux.tm_isdst;
    /* Windows的struct tm没有tm_gmtoff，把本地时间当成UTC反解即可得到偏移
     * The struct tm of Windows has no tm_gmtoff, interpreting the local time as if
     * it were UTC gives the offset back */
    _local_gmtoff.store((long)(_mkgmtime(&aux) - t), std::memory_order_relaxed);
#else
    localtime_r(&t, &aux);
    _local_gmtoff.store(aux.tm_gmtoff, std::memory_order_relaxed);
    if (aux.tm_zone && aux.tm_zone[0]) {
        /* 写入当前未被使用的那块缓冲，再以release发布索引，与读侧的acquire配对
         * Write into whichever buffer is not in use and publish the index with a release,
         * paired with the acquire on the reading side */
        auto next = 1 - _zone_index.load(std::memory_order_relaxed);
        strncpy(_zone_name[next], aux.tm_zone, sizeof(_zone_name[next]) - 1);
        _zone_name[next][sizeof(_zone_name[next]) - 1] = '\0';
        _zone_index.store(next, std::memory_order_release);
    }
#endif
#ifdef _WIN32
    _daylight_active.store(isdst > 0 ? 1 : 0, std::memory_order_relaxed);
#else
    _daylight_active.store(aux.tm_isdst > 0 ? 1 : 0, std::memory_order_relaxed);
#endif
}
} // namespace toolkit
