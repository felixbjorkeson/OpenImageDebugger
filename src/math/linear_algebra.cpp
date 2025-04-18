/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2025 OpenImageDebugger contributors
 * (https://github.com/OpenImageDebugger/OpenImageDebugger)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "linear_algebra.h"

#include <iostream>


namespace oid
{


vec4::vec4(const float x, const float y, const float z, const float w)
    : vec_{x, y, z, w}
{
}


vec4& vec4::operator+=(const vec4& b)
{
    for (int i = 0; i < 4; ++i) {
        vec_[i] += b.vec_[i];
    }
    return *this;
}


void vec4::print() const
{
    std::cout << vec_.transpose() << std::endl;
}


float* vec4::data()
{
    return vec_.data();
}


float& vec4::x()
{
    return vec_[0];
}


float& vec4::y()
{
    return vec_[1];
}


float& vec4::z()
{
    return vec_[2];
}


float& vec4::w()
{
    return vec_[3];
}


const float& vec4::x() const
{
    return vec_[0];
}


const float& vec4::y() const
{
    return vec_[1];
}


const float& vec4::z() const
{
    return vec_[2];
}


const float& vec4::w() const
{
    return vec_[3];
}


vec4 vec4::zero()
{
    return {0.0f, 0.0f, 0.0f, 0.0f};
}


vec4 operator-(const vec4& vector)
{
    return {-vector.x(), -vector.y(), -vector.z(), -vector.w()};
}


void mat4::set_identity()
{
    // clang-format off
    *this << std::initializer_list<float>{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    // clang-format on
}


void mat4::set_from_st(const float scaleX,
                       const float scaleY,
                       const float scaleZ,
                       const float x,
                       const float y,
                       const float z)
{
    float* data = this->data();

    data[0]  = scaleX;
    data[5]  = scaleY;
    data[10] = scaleZ;
    data[12] = x;
    data[13] = y;
    data[14] = z;

    data[1] = data[2] = data[3] = data[4] = 0.0f;
    data[6] = data[7] = data[8] = data[9] = 0.0f;
    data[11]                              = 0.0f;
    data[15]                              = 1.0f;
}


void mat4::set_from_srt(const float scaleX,
                        const float scaleY,
                        const float scaleZ,
                        const float rZ,
                        const float x,
                        const float y,
                        const float z)
{
    using Eigen::Affine3f;
    using Eigen::AngleAxisf;
    using Eigen::Vector3f;

    auto t = Affine3f::Identity();
    t.translate(Vector3f(x, y, z))
        .rotate(AngleAxisf(rZ, Vector3f(0.0f, 0.0f, 1.0f)))
        .scale(Vector3f(scaleX, scaleY, scaleZ));
    this->mat_ = t.matrix();
}


float* mat4::data()
{
    return mat_.data();
}


void mat4::operator<<(const std::initializer_list<float>& data)
{
    memcpy(mat_.data(), data.begin(), sizeof(float) * data.size());
}


mat4 mat4::rotation(const float angle)
{
    using Eigen::Affine3f;
    using Eigen::AngleAxisf;
    using Eigen::Vector3f;

    auto result = mat4{};
    auto t      = Affine3f::Identity();
    t.rotate(AngleAxisf(angle, Vector3f(0.0f, 0.0f, 1.0f)));

    result.mat_ = t.matrix();
    return result;
}


mat4 mat4::translation(const vec4& vector)
{
    using Eigen::Affine3f;
    using Eigen::Vector3f;

    auto result = mat4{};

    auto t = Affine3f::Identity();
    t.translate(Vector3f(vector.x(), vector.y(), vector.z()));
    result.mat_ = t.matrix();

    return result;
}


mat4 mat4::scale(const vec4& factor)
{
    using Eigen::Affine3f;
    using Eigen::Vector3f;

    auto result = mat4{};

    auto t = Affine3f::Identity();
    t.scale(Vector3f(factor.x(), factor.y(), factor.z()));
    result.mat_ = t.matrix();

    return result;
}


void mat4::set_ortho_projection(const float right,
                                const float top,
                                const float near,
                                const float far)
{
    const auto data = this->data();

    data[0]  = 1.0f / right;
    data[5]  = -1.0f / top;
    data[10] = -2.0f / (far - near);
    data[14] = -(far + near) / (far - near);

    data[1] = data[2] = data[3] = data[4] = 0.0f;
    data[6] = data[7] = data[8] = data[9] = 0.0f;
    data[11] = data[12] = data[13] = 0.0f;
    data[15]                       = 1.0f;
}


void mat4::print() const
{
    std::cout << mat_ << std::endl;
}


mat4 mat4::inv() const
{
    auto result = mat4{};
    result.mat_ = this->mat_.inverse();

    return result;
}


float& mat4::operator()(const int row, const int col)
{
    return mat_(row, col);
}

vec4 mat4::operator*(const vec4& vec) const
{
    auto result = vec4{};
    result.vec_ = this->mat_ * vec.vec_;

    return result;
}

} // namespace oid
