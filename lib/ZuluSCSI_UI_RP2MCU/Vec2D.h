#include <cmath>
#include <iostream>

#include "ZuluSCSI_log.h"

struct Vec2D 
{
    float x;
    float y;

    // Constructor
    Vec2D(float _x = 0.0f, float _y = 0.0f) : x(_x), y(_y) {}

    // Constructor from two Vec2D points: vector from p1 to p2
    Vec2D(const Vec2D& p1, const Vec2D& p2)
        : x(p2.x - p1.x), y(p2.y - p1.y) {}

    // Constructor from raw coordinates of two points: vector from (x1,y1) to (x2,y2)
    Vec2D(float x1, float y1, float x2, float y2)
        : x(x2 - x1), y(y2 - y1) {}

    // Explicit setter
    void set(float _x, float _y) 
    {
        x = _x;
        y = _y;
    }

    // Explicit setter (from another Vec2D)
    void set(const Vec2D& other) {
        x = other.x;
        y = other.y;
    }

    // Vector addition
    Vec2D operator+(const Vec2D& other) const {
        return Vec2D(x + other.x, y + other.y);
    }

    // Vector subtraction
    Vec2D operator-(const Vec2D& other) const {
        return Vec2D(x - other.x, y - other.y);
    }

    // Scalar multiplication
    Vec2D operator*(float scalar) const {
        return Vec2D(x * scalar, y * scalar);
    }

    // Scalar division
    Vec2D operator/(float scalar) const {
        return Vec2D(x / scalar, y / scalar);
    }

    // Compound operators
    Vec2D& operator+=(const Vec2D& other) {
        x += other.x;
        y += other.y;
        return *this;
    }

    Vec2D& operator-=(const Vec2D& other) {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    Vec2D& operator*=(float scalar) {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    Vec2D& operator/=(float scalar) {
        x /= scalar;
        y /= scalar;
        return *this;
    }

    // Magnitude (length)
    float magnitude() const {
        return std::sqrt(x * x + y * y);
    }

    // Normalize (unit vector)
    Vec2D normalized() const {
        float mag = magnitude();
        return (mag > 0) ? *this / mag : Vec2D(0, 0);
    }

    // Dot product
    float dot(const Vec2D& other) const {
        return x * other.x + y * other.y;
    }

    // Cross product (in 2D returns scalar "z-component")
    float cross(const Vec2D& other) const {
        return x * other.y - y * other.x;
    }

    // Print helper
    void print() const {
        std::cout << "(" << x << ", " << y << ")\n";
    }
};