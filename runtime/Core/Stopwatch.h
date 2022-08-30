/*
Copyright(c) 2016-2022 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

//= INCLUDES ==================
#include <chrono>
#include "SpartanDefinitions.h"
//=============================

namespace Spartan
{
    class SP_CLASS Stopwatch
    {
    public:
        Stopwatch() { Start(); }
        ~Stopwatch() = default;

        void Start()
        {
            m_start = std::chrono::high_resolution_clock::now();
        }

        float GetElapsedTimeSec() const
        {
            const std::chrono::duration<double, std::milli> ms = std::chrono::high_resolution_clock::now() - m_start;
            return static_cast<float>(ms.count() / 1000);
        }

        float GetElapsedTimeMs() const
        {
            const std::chrono::duration<double, std::milli> ms = std::chrono::high_resolution_clock::now() - m_start;
            return static_cast<float>(ms.count());
        }

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> m_start;
    };
}
