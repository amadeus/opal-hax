// gestures: recognizes Opal's three hand gestures from the camera's 21 hand landmarks. Opal.app classifies with a
// random-forest model on the Mac; this uses simple finger geometry instead:
//   draw  - only the index finger extended: the fingertip draws a fading line
//   peace - index + middle extended, others folded: a short peace-sign burst
//   pinch - thumb tip touching index tip: toggles zoom
#pragma once
#include <chrono>
#include <cmath>
#include <deque>

#include "../core/opal_camera.hpp"

namespace opal {

struct StrokePoint {
    float x, y;  // normalized camera frame
    double t;
    bool penDown;
};

class Gestures {
   public:
    enum class Kind { None, Point, Peace, Pinch };

    // landmarks arrive in the hand-pose manager's padded square space; convert to the 16:9 frame
    static void toFrame(const Hand& h, int i, float& x, float& y) {
        x = h.landmarks[i][0];
        y = (h.landmarks[i][1] * 3840.f - 840.f) / 2160.f;
    }

    Kind classify(const Hand& h) const {
        if(!h.valid || h.score < 0.5f) return Kind::None;
        auto d = [&](int a, int b) {
            float ax, ay, bx, by;
            toFrame(h, a, ax, ay);
            toFrame(h, b, bx, by);
            return std::hypot(ax - bx, (ay - by) * 9.f / 16.f);
        };
        float palm = d(0, 9);  // wrist to middle knuckle
        if(palm <= 0) return Kind::None;
        auto extended = [&](int tip, int pip) { return d(tip, 0) > d(pip, 0) * 1.15f; };
        bool idx = extended(8, 6), mid = extended(12, 10), ring = extended(16, 14), pinky = extended(20, 18);
        if(d(4, 8) < palm * 0.25f) return Kind::Pinch;
        if(idx && mid && !ring && !pinky) return Kind::Peace;
        if(idx && !mid && !ring && !pinky) return Kind::Point;
        return Kind::None;
    }

    // returns events; keeps the drawing stroke
    void update(const Hand& h, bool drawOn, bool peaceOn, bool pinchOn, double now, bool& peaceEvent, bool& pinchEvent) {
        peaceEvent = pinchEvent = false;
        Kind k = classify(h);
        stable_ = (k == last_) ? stable_ + 1 : 0;
        last_ = k;
        if(drawOn && k == Kind::Point && stable_ >= 2) {
            float x, y;
            toFrame(h, 8, x, y);
            stroke.push_back({x, y, now, !stroke.empty() && now - stroke.back().t < 0.25});
        }
        while(!stroke.empty() && now - stroke.front().t > 3.0) stroke.pop_front();  // fade after 3 s
        if(peaceOn && k == Kind::Peace && stable_ == 4 && now - lastPeace_ > 2.0) {
            peaceEvent = true;
            lastPeace_ = now;
            toFrame(h, 9, peaceX, peaceY);
        }
        if(pinchOn && k == Kind::Pinch && stable_ == 3 && now - lastPinch_ > 1.5) {
            pinchEvent = true;
            lastPinch_ = now;
        }
    }

    std::deque<StrokePoint> stroke;
    float peaceX = 0.5f, peaceY = 0.5f;

   private:
    Kind last_ = Kind::None;
    int stable_ = 0;
    double lastPeace_ = -10, lastPinch_ = -10;
};

}  // namespace opal
