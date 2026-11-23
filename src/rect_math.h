// rect_math.h: wxRect's Intersect() and Contains() as free functions (#115, slice S8.1).
//
// wxRect's own Intersect() and Contains() are not inline: they live in the wx core (GUI) library, which the daemon
// stops linking in #115 step 8. These compute the same results, so model code keeps wxRect, wxPoint and wxSize as
// plain value types (their inline members need nothing from wx core). They take any rect type with int x, y, width
// and height, and any point type with int x and y, which lets the unit test check them without wx.
#ifndef RECT_MATH_H_INCLUDED
#define RECT_MATH_H_INCLUDED

// The part of a that is inside b, the way wxRect::Intersect() computes it. When they do not overlap the result is
// empty: width and height are 0, and x and y keep their clipped values.
template<typename Rect>
Rect RectIntersect(Rect a, const Rect& b)
{
    int right = a.x + a.width - 1;
    int bottom = a.y + a.height - 1;
    if (a.x < b.x)
        a.x = b.x;
    if (a.y < b.y)
        a.y = b.y;
    if (right > b.x + b.width - 1)
        right = b.x + b.width - 1;
    if (bottom > b.y + b.height - 1)
        bottom = b.y + b.height - 1;
    a.width = right - a.x + 1;
    a.height = bottom - a.y + 1;
    if (a.width <= 0 || a.height <= 0)
    {
        a.width = 0;
        a.height = 0;
    }
    return a;
}

// Whether pt is inside r, the way wxRect::Contains(const wxPoint&) decides it.
template<typename Rect, typename Point>
bool RectContains(const Rect& r, const Point& pt)
{
    return pt.x >= r.x && pt.y >= r.y && pt.y - r.y < r.height && pt.x - r.x < r.width;
}

#endif // RECT_MATH_H_INCLUDED
