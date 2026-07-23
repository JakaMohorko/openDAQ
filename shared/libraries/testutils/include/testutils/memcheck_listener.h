/*
 * Copyright 2022-2026 openDAQ d.o.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifndef NDEBUG
    #ifdef _MSC_VER
        #define _CRTDBG_MAP_ALLOC
        #include <crtdbg.h>
    #endif // _MSC_VER
#endif // !NDEBUG

#include <testutils/base_test_listener.h>

#ifndef NDEBUG
#ifdef _MSC_VER
/*
int AllocHook(
    int allocType, void* userData, size_t size, int blockType, long requestNumber, const unsigned char* filename, int lineNumber)
{
    return 1;
}
*/

#endif
#endif

class MemCheckListener : public BaseTestListener
{
public:
    inline static bool expectMemoryLeak = false;

    // Tolerance for the whole-process MSVC-CRT leak check (_CrtMemDifference), in net allocated
    // blocks. Some openDAQ subsystems keep a small, fixed amount of process-lifetime state whose
    // construction/teardown is driven by module (DLL) load/unload and is therefore not aligned with
    // per-test boundaries - most notably the module manager's own boost::dll bookkeeping for a loaded
    // module (see OrphanedModules), which allocates/frees a handle object as an Instance comes and
    // goes. Because the CRT checkpoint spans the whole process it attributes that churn to whichever
    // test's window it lands in, producing false positives (often as a NEGATIVE delta - a test that
    // *freed* blocks - which cannot be a leak). Binaries that create/destroy many Instances (e.g. the
    // docs examples) can raise this tolerance so a handful of such churned blocks are not reported as
    // leaks. It only relaxes the raw CRT block-count check; the openDAQ object-count check in
    // DaqMemCheckListener (which tracks every IBaseObject and is the authoritative leak detector)
    // is unaffected and still fails on any real openDAQ object leak. Default 0 preserves the strict,
    // historical behaviour for every other test binary.
    inline static long crtLeakToleranceBlocks = 0;

protected:
    void OnTestStart(const testing::TestInfo& info) override
    {
#ifndef NDEBUG
#ifdef _MSC_VER
        //        _CrtSetAllocHook(AllocHook);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtMemCheckpoint(&state1);
#elif defined(__MINGW32__)
       //curBytesAllocated = getBytesAllocated();
#endif
#endif
        expectMemoryLeak = false;
        BaseTestListener::OnTestStart(info);
    }

    void OnTestEnd(const testing::TestInfo& info) override
    {
        if (info.result()->Passed())
        {
#ifndef NDEBUG
#ifdef _MSC_VER
            _CrtMemState state2, state3;
            _CrtMemCheckpoint(&state2);

            int crtMemDifference = _CrtMemDifference(&state3, &state1, &state2);
            if (expectMemoryLeak)
            {
                if (!crtMemDifference)
                {
                    FAIL() << "Memory leaks expected, but not detected";
                }
            }
            else if (crtMemDifference)
            {
                if (crtLeakToleranceBlocks <= 0)
                {
                    // Default (historical) behaviour: any heap difference fails the test.
                    //            _CrtMemDumpAllObjectsSince(&state1);
                    FAIL() << "Memory leaks detected (" << state3.lTotalCount << " allocations)";
                }
                else
                {
                    // A leak grows the heap: with a tolerance configured, only a positive net block
                    // count that exceeds it is treated as a leak. A negative or zero net delta cannot
                    // be a leak (the test freed as much as, or more than, it allocated in its window).
                    const long netBlocks = state3.lCounts[_NORMAL_BLOCK] + state3.lCounts[_CRT_BLOCK];
                    if (netBlocks > crtLeakToleranceBlocks)
                        FAIL() << "Memory leaks detected (" << netBlocks << " net blocks, " << state3.lTotalCount << " allocations)";
                }
            }
#elif defined(__MINGW32__)
            /*if (expectMemoryLeak)
                  ASSERT_NE(getBytesAllocated(), curBytesAllocated) << "Memory leaks expected, but not detected";
              else
                  ASSERT_EQ(getBytesAllocated(), curBytesAllocated) << "Memory leaks detected";
            */
#endif
#endif
        }
    }

private:
#ifndef NDEBUG
    #ifdef _MSC_VER
        _CrtMemState state1;
    #elif defined(__MINGW32__)
        size_t curBytesAllocated;
    #endif
#endif
};
