#pragma once

#include <algorithm>

namespace opal {

struct EdgeMouseMotion {
    double x = 0.0;
    double y = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    bool relative = false;
    bool enter_relative = false;
    bool leave_relative = false;
};

class EdgeMouseTracker {
public:
    void reset(double x, double y, int width, int height)
    {
        x_ = clamp_axis(x, width);
        y_ = clamp_axis(y, height);
        relative_ = false;
    }

    EdgeMouseMotion move(double absolute_x, double absolute_y, double dx, double dy,
                         int width, int height)
    {
        EdgeMouseMotion motion{};
        motion.dx = dx;
        motion.dy = dy;

        if (width <= 0 || height <= 0) {
            reset(0.0, 0.0, width, height);
            return motion;
        }

        if (relative_) {
            x_ += dx;
            y_ += dy;
            motion.x = x_;
            motion.y = y_;
            motion.relative = true;
            if (inside(x_, y_, width, height)) {
                relative_ = false;
                motion.leave_relative = true;
            }
            return motion;
        }

        const double x = clamp_axis(absolute_x, width);
        const double y = clamp_axis(absolute_y, height);
        const double max_x = static_cast<double>(width - 1);
        const double max_y = static_cast<double>(height - 1);
        const bool outward = (x <= 0.0 && dx < 0.0) || (x >= max_x && dx > 0.0) ||
                             (y <= 0.0 && dy < 0.0) || (y >= max_y && dy > 0.0);

        if (!outward) {
            x_ = x;
            y_ = y;
            motion.x = x_;
            motion.y = y_;
            return motion;
        }

        x_ += dx;
        y_ += dy;
        relative_ = true;
        motion.x = x_;
        motion.y = y_;
        motion.relative = true;
        motion.enter_relative = true;
        return motion;
    }

    bool relative() const { return relative_; }

private:
    static double clamp_axis(double value, int extent)
    {
        if (extent <= 1) return 0.0;
        return std::clamp(value, 0.0, static_cast<double>(extent - 1));
    }

    static bool inside(double x, double y, int width, int height)
    {
        if (width <= 0 || height <= 0) return true;
        return x >= 0.0 && x <= static_cast<double>(width - 1) &&
               y >= 0.0 && y <= static_cast<double>(height - 1);
    }

    double x_ = 0.0;
    double y_ = 0.0;
    bool relative_ = false;
};

}
