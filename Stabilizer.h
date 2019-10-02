#pragma once

#include <opencv2/opencv.hpp>

struct TransformParam;

struct Trajectory
{
    Trajectory() = default;
    Trajectory(double _x, double _y, double _a) {
        x = _x;
        y = _y;
        a = _a;
    }
    // "+"
    friend Trajectory operator+(const Trajectory &c1, const Trajectory  &c2) {
        return Trajectory(c1.x + c2.x, c1.y + c2.y, c1.a + c2.a);
    }
    //"-"
    friend Trajectory operator-(const Trajectory &c1, const Trajectory  &c2) {
        return Trajectory(c1.x - c2.x, c1.y - c2.y, c1.a - c2.a);
    }
    //"*"
    friend Trajectory operator*(const Trajectory &c1, const Trajectory  &c2) {
        return Trajectory(c1.x*c2.x, c1.y*c2.y, c1.a*c2.a);
    }
    //"/"
    friend Trajectory operator/(const Trajectory &c1, const Trajectory  &c2) {
        return Trajectory(c1.x / c2.x, c1.y / c2.y, c1.a / c2.a);
    }
    //"="
    Trajectory operator =(const Trajectory &rx) {
        x = rx.x;
        y = rx.y;
        a = rx.a;
        return Trajectory(x, y, a);
    }

    double x;
    double y;
    double a; // angle
};


class Stabilizer {
public:
    Stabilizer();
    void operator()(cv::Mat& cur);

    Stabilizer(const Stabilizer&) = delete;
    Stabilizer& operator=(const Stabilizer&) = delete;

private:
    std::ofstream out_transform;
    std::ofstream out_trajectory;
    std::ofstream out_smoothed_trajectory;
    std::ofstream out_new_transform;

    cv::Mat cur_grey;
    cv::Mat prev;
    cv::Mat prev_grey;

    cv::Mat last_T;

    Trajectory X;//posteriori state estimate
    Trajectory	X_;//priori estimate
    Trajectory P;// posteriori estimate error covariance
    Trajectory P_;// priori estimate error covariance
    Trajectory K;//gain

    double a = 0;
    double x = 0;
    double y = 0;

    int k = 0;
};