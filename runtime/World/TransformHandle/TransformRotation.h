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

//= INCLUDES =================
#include "TransformOperator.h"
//============================

namespace Spartan
{
    class SP_CLASS TransformRotation : public TransformOperator
    {
    public:
        TransformRotation(Context* context = nullptr);
        ~TransformRotation() = default;

    protected:
        void InteresectionTest(const Math::Ray& mouse_ray) override;
        void ComputeDelta(const Math::Ray& mouse_ray, const Camera* camera) override;
        void MapToTransform(Transform* transform, const TransformHandleSpace space) override;

    private:
        Math::Vector3 m_initial_direction;
        Math::Vector3 m_intersection_axis;
        float m_previous_angle = 0.0f;
        float m_angle_delta    = 0.0f;
    };
}
