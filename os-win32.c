/*
 * os-win32.c
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2010 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <windows.h>
#include <mmsystem.h>
#include "system/runstate.h"

static BOOL WINAPI qemu_ctrl_handler(DWORD type)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
    /* Windows 7 kills application when the function returns.
       Sleep here to give QEMU a try for closing.
       Sleep period is 10000ms because Windows kills the program
       after 10 seconds anyway. */
    Sleep(10000);

    return TRUE;
}

static TIMECAPS mm_tc;

static void os_undo_timer_resolution(void)
{
    timeEndPeriod(mm_tc.wPeriodMin);
}

/*
 * Windows 11 stops honoring timeBeginPeriod for a process whose windows are all
 * minimized or covered: its timer waits fall back to the 15.6 ms system tick.
 * Measured with a probe on 11 22631, Sleep(1) went from 1.8 ms to 15.7 ms a few
 * seconds after its window was minimized; for QEMU every guest timer then fires
 * late, and the A5 display's 60 Hz VBL came every 31 ms (docs/windows-gl.md).
 * PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION in the control mask with the
 * state bit clear keeps the requested resolution. Defined here: the SDK guards
 * the declarations behind _WIN32_WINNT >= Windows 10, newer than QEMU builds for.
 * Older Windows has no such class and the call fails harmlessly.
 */
static void os_keep_timer_resolution(void)
{
    struct {
        ULONG Version, ControlMask, StateMask;
    } state = { 1, 0x4 /* IGNORE_TIMER_RESOLUTION */, 0 };
    BOOL (WINAPI *set_info)(HANDLE, int, LPVOID, DWORD) =
        (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");

    if (set_info) {
        set_info(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &state, sizeof(state));
    }
}

void os_setup_early_signal_handling(void)
{
    SetConsoleCtrlHandler(qemu_ctrl_handler, TRUE);
    timeGetDevCaps(&mm_tc, sizeof(mm_tc));
    timeBeginPeriod(mm_tc.wPeriodMin);
    os_keep_timer_resolution();
    atexit(os_undo_timer_resolution);
}

void os_set_line_buffering(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
}
