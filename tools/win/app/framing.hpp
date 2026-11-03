// framing: Opal's AppearancePipeline geometry (OpalCameraVideoService ZoomFilter, reverse-engineered; see
// tools/win/verify/README.md): Face Lock crop, Zoom crop and the Spotlight circle, each with its own on/off ramp, and
// Face Lock / Spotlight each following the faces through their own 4-state Kalman filter.
//
// Everything is in camera-frame pixels (W x H). Ramps advance per rendered frame (Opal counts frames, not time).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "../core/opal_camera.hpp"

namespace opal {

struct CropRect {
    bool on = false;
    double x = 0, y = 0, w = 0, h = 0;  // pixels in the W x H frame
};

// KalmanFilter (Swift package) with F = H = I, B = 0, Q = 0.002 I, R = I; state [x, y, w, h]
struct FaceKalman {
    double x[4] = {0, 0, 0, 0};
    double P[4][4];
    FaceKalman() {
        for(auto& r : P)
            for(double& v : r) v = 1;  // Opal starts from a matrix of ones
    }
    // predict: x' = x, P' = P + Q
    static void predict(const FaceKalman& in, FaceKalman& out) {
        out = in;
        for(int i = 0; i < 4; i++) out.P[i][i] += 0.002;
    }
    // update with measurement z: K = P (P + R)^-1, x += K (z - x), P = (I - K) P
    static void update(const FaceKalman& in, const double z[4], FaceKalman& out) {
        double S[4][8];
        for(int i = 0; i < 4; i++)
            for(int j = 0; j < 4; j++) {
                S[i][j] = in.P[i][j] + (i == j ? 1.0 : 0.0);
                S[i][4 + j] = i == j ? 1.0 : 0.0;
            }
        for(int c = 0; c < 4; c++) {  // Gauss-Jordan
            int piv = c;
            for(int r = c + 1; r < 4; r++)
                if(std::fabs(S[r][c]) > std::fabs(S[piv][c])) piv = r;
            if(piv != c)
                for(int k = 0; k < 8; k++) std::swap(S[c][k], S[piv][k]);
            double d = S[c][c];
            if(d == 0) return void(out = in);
            for(int k = 0; k < 8; k++) S[c][k] /= d;
            for(int r = 0; r < 4; r++)
                if(r != c) {
                    double f = S[r][c];
                    for(int k = 0; k < 8; k++) S[r][k] -= f * S[c][k];
                }
        }
        double K[4][4];
        for(int i = 0; i < 4; i++)
            for(int j = 0; j < 4; j++) {
                K[i][j] = 0;
                for(int k = 0; k < 4; k++) K[i][j] += in.P[i][k] * S[k][4 + j];
            }
        out = in;
        for(int i = 0; i < 4; i++)
            for(int j = 0; j < 4; j++) out.x[i] += K[i][j] * (z[j] - in.x[j]);
        for(int i = 0; i < 4; i++)
            for(int j = 0; j < 4; j++) {
                double v = in.P[i][j];
                for(int k = 0; k < 4; k++) v -= K[i][k] * in.P[k][j];
                out.P[i][j] = v;
            }
    }
};

class ZoomStage {
   public:
    enum Type { Zoom, FaceZoom, Spotlight };
    explicit ZoomStage(Type t) : type_(t) {}
    float ramp = 0;  // currentZoomIn, 0..1
    bool active = false;
    double amount = 0;  // adjustedPadding (OCZoom / OCFaceLock)

    // per rendered frame, before processing (frame fn: +-0.05 per frame)
    void step() {
        if((ramp < 1 && active) || (ramp > 0 && !active)) ramp = active ? ramp + 0.05f : ramp - 0.05f;
    }
    bool processing() const { return active || ramp > 0; }

    // target rect (vt44): union of faces >= 0.75 confidence through the Kalman filter; zoom: fixed centre box
    void target(const std::vector<Face>& faces, double W, double H, double r[4]) {
        if(type_ == Zoom) {
            r[0] = 0.4 * W, r[1] = 0.4 * H, r[2] = 0.2 * W, r[3] = 0.2 * H;
            return;
        }
        bool any = false;
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for(auto& f : faces) {
            if(f.confidence < 0.75f) continue;
            double fx = (double)f.xmin * W, fy = (double)f.ymin * H, fw = ((double)f.xmax - f.xmin) * W, fh = ((double)f.ymax - f.ymin) * H;
            if(!any) x0 = fx, y0 = fy, x1 = fx + fw, y1 = fy + fh, any = true;
            else x0 = std::min(x0, fx), y0 = std::min(y0, fy), x1 = std::max(x1, fx + fw), y1 = std::max(y1, fy + fh);
        }
        if(!any) {  // hold the last estimate (all zero before the first face: Opal parks at the top-left)
            r[0] = kf_.x[0], r[1] = kf_.x[1], r[2] = kf_.x[3] * 16 / 9, r[3] = kf_.x[3];
            return;
        }
        double z[4] = {x0, y0, x1 - x0, y1 - y0};
        FaceKalman pred, upd;
        FaceKalman::predict(kf_, pred);
        FaceKalman::update(pred, z, upd);
        if(std::fabs(upd.x[2] - z[2]) < 70.0) {
            double z2[4] = {z[0], z[1], z[2], pred.x[3]};
            FaceKalman::update(pred, z2, kf_);
            double h = pred.x[3];
            r[0] = kf_.x[0], r[1] = kf_.x[1], r[2] = h * 16 / 9, r[3] = h;
        } else {
            kf_ = upd;
            r[0] = upd.x[0], r[1] = upd.x[1], r[2] = upd.x[3] * 16 / 9, r[3] = upd.x[3];
        }
    }

    // crop for Zoom / Face Lock (vt45)
    CropRect crop(const std::vector<Face>& faces, double W, double H) {
        CropRect c;
        if(!processing()) return c;
        double R[4];
        target(faces, W, H, R);
        if(type_ == FaceZoom) R[0] = std::max(R[0] - R[2] / 4, 0.0);
        double k = 1 + 1.5 * amount, cw = W / k, ch = H / k;
        double r1x = std::max(R[0] + R[2] / 2 - cw / 2, 0.0), r1y = std::max(R[1] + R[3] / 2 - ch / 2, 0.0);
        double r2x = std::max(std::min(r1x, W - cw), 0.0), r2y = std::max(std::min(r1y, H - ch), 0.0);
        double r3w = std::min(cw, W), r3h = std::min(ch, H);
        float z = std::clamp(ramp, 0.f, 1.f);
        float t = z * z * (3 - 2 * z);
        c.on = true;
        c.x = t * (float)r2x;
        c.y = t * (float)r2y;
        c.w = (float)W - ((float)W - (float)r3w) * t;
        c.h = (float)H - ((float)H - (float)r3h) * t;
        return c;
    }

    // spotlight circle (position normalized, radius uniform 11 -> 0.5)
    bool spotlight(const std::vector<Face>& faces, double W, double H, float& px, float& py, float& radius) {
        if(!processing()) return false;
        double R[4];
        target(faces, W, H, R);
        px = (float)((R[0] + R[2] / 4) / W);
        py = (float)((R[1] + R[3] / 2) / H);
        radius = 11.f - 10.5f * ramp;
        return true;
    }

   private:
    Type type_;
    FaceKalman kf_;
};

class Framing {
   public:
    ZoomStage faceZoom{ZoomStage::FaceZoom}, zoom{ZoomStage::Zoom}, spot{ZoomStage::Spotlight};

    struct Result {
        CropRect face, zoom;
        bool spotlight = false;
        float spotX = 0, spotY = 0, spotRadius = 11;
        // both crops combined, normalized (for drawing camera-space points on the output)
        float cx = 0, cy = 0, cw = 1, ch = 1;
    };

    // once per rendered frame
    Result update(bool zoomOn, float zoomAmount, bool faceLockOn, float faceLockAmount, bool spotlightOn, const std::vector<Face>& faces, int W, int H) {
        faceZoom.active = faceLockOn, faceZoom.amount = faceLockAmount;
        zoom.active = zoomOn, zoom.amount = zoomAmount;
        spot.active = spotlightOn;
        faceZoom.step(), zoom.step(), spot.step();
        Result r;
        r.face = faceZoom.crop(faces, W, H);
        r.zoom = zoom.crop(faces, W, H);
        r.spotlight = spot.spotlight(faces, W, H, r.spotX, r.spotY, r.spotRadius);
        double x = 0, y = 0, w = 1, h = 1;
        for(const CropRect* c : {&r.face, &r.zoom})
            if(c->on) {
                x += c->x / W * w, y += c->y / H * h;
                w *= c->w / W, h *= c->h / H;
            }
        r.cx = (float)x, r.cy = (float)y, r.cw = (float)w, r.ch = (float)h;
        return r;
    }
};

}  // namespace opal
