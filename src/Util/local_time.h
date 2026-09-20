//
// Created by alex on 2022/5/29.
//

#ifndef UTIL_LOCALTIME_H
#define UTIL_LOCALTIME_H
#include <time.h>

namespace toolkit {
void no_locks_localtime(struct tm *tmp, time_t t);
void local_time_init();
/* 刷新缓存的时区偏移与夏令时状态，使夏令时切换后的本地时间依然正确。
 * no_locks_localtime()用的就是这份缓存，长时间运行的进程必须定期调用本函数，
 * 否则夏令时切换后取到的本地时间会一直偏差(ZLToolKit内部由getGMTOff()/getLocalTime()
 * 每60秒触发一次)。该函数内部调用localtime_r()/localtime_s()，会加锁且不是fork安全的，
 * 故调用方必须避开热点路径并控制调用频次。
 * Refresh the cached timezone offset and daylight saving flag, so that the local time
 * stays correct after a daylight saving time switch. no_locks_localtime() reads that
 * very cache, so a long running process has to call this periodically, otherwise the
 * local time would stay off after a switch (inside ZLToolKit it is triggered once every
 * 60 seconds by getGMTOff()/getLocalTime()). This function calls localtime_r() or
 * localtime_s(), which takes a lock and is not fork() friendly, so callers must keep it
 * out of the hot path and call it at a low rate. */
void local_time_refresh();
int get_daylight_active();
/* 获取本地时间相对UTC的偏移，单位为秒，取自localtime()填充的tm_gmtoff，已含夏令时修正
 * Offset of the local time from UTC in seconds, taken from the tm_gmtoff filled in by
 * localtime(), daylight saving time included. */
long get_local_gmtoff();

} // namespace toolkit
#endif // UTIL_LOCALTIME_H
