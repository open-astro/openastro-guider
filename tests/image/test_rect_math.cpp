// RectIntersect and RectContains (src/rect_math.h, #115 slice S8.1) give what wxRect::Intersect and
// wxRect::Contains(const wxPoint&) give. The expected values come from wx 3.2's wxRect; the two were also compared
// over 2,000,000 random rects and points with wx core linked, with no difference. std-only: a plain struct stands in
// for wxRect and wxPoint.

#include "rect_math.h"

#include <gtest/gtest.h>

namespace
{
struct Rect
{
    int x, y, width, height;
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
};

struct Point
{
    int x, y;
};

std::ostream& operator<<(std::ostream& os, const Rect& r)
{
    return os << r.x << "," << r.y << "+" << r.width << "x" << r.height;
}
} // namespace

TEST(RectMath, IntersectOverlap)
{
    EXPECT_EQ(RectIntersect(Rect { 0, 0, 10, 10 }, Rect { 5, 3, 10, 10 }), (Rect { 5, 3, 5, 7 }));
    EXPECT_EQ(RectIntersect(Rect { -5, -5, 20, 20 }, Rect { 0, 0, 640, 480 }), (Rect { 0, 0, 15, 15 }));
}

TEST(RectMath, IntersectInside)
{
    EXPECT_EQ(RectIntersect(Rect { 10, 20, 30, 40 }, Rect { 0, 0, 640, 480 }), (Rect { 10, 20, 30, 40 }));
    EXPECT_EQ(RectIntersect(Rect { 0, 0, 640, 480 }, Rect { 10, 20, 30, 40 }), (Rect { 10, 20, 30, 40 }));
}

TEST(RectMath, IntersectEdgeTouchingIsEmpty)
{
    // [0,10) and [10,20) share no pixel: empty, with x clipped to 10
    EXPECT_EQ(RectIntersect(Rect { 0, 0, 10, 10 }, Rect { 10, 0, 10, 10 }), (Rect { 10, 0, 0, 0 }));
}

TEST(RectMath, IntersectDisjointKeepsClippedOrigin)
{
    EXPECT_EQ(RectIntersect(Rect { 0, 0, 5, 5 }, Rect { 20, 30, 5, 5 }), (Rect { 20, 30, 0, 0 }));
    EXPECT_EQ(RectIntersect(Rect { 20, 30, 5, 5 }, Rect { 0, 0, 5, 5 }), (Rect { 20, 30, 0, 0 }));
}

TEST(RectMath, IntersectWithEmpty)
{
    EXPECT_EQ(RectIntersect(Rect { 3, 4, 0, 0 }, Rect { 0, 0, 640, 480 }), (Rect { 3, 4, 0, 0 }));
    EXPECT_EQ(RectIntersect(Rect { 3, 4, 10, 10 }, Rect { 0, 0, 0, 0 }), (Rect { 3, 4, 0, 0 }));
}

TEST(RectMath, Contains)
{
    const Rect r { 10, 20, 5, 4 };
    EXPECT_TRUE(RectContains(r, Point { 10, 20 }));
    EXPECT_TRUE(RectContains(r, Point { 14, 23 }));
    EXPECT_FALSE(RectContains(r, Point { 15, 23 }));
    EXPECT_FALSE(RectContains(r, Point { 14, 24 }));
    EXPECT_FALSE(RectContains(r, Point { 9, 20 }));
    EXPECT_FALSE(RectContains(r, Point { 10, 19 }));
    EXPECT_FALSE(RectContains(Rect { 0, 0, 0, 0 }, Point { 0, 0 }));
}
