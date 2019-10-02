#include "Stabilizer.h"


/*
Thanks Nghia Ho for his excellent code.
And,I modified the smooth step using a simple kalman filter .
So,It can processes live video streaming.
modified by chen jia.
email:chenjia2013@foxmail.com
*/


// This video stablisation smooths the global trajectory using a sliding average window

//const int SMOOTHING_RADIUS = 15; // In frames. The larger the more stable the video, but less reactive to sudden panning
const int HORIZONTAL_BORDER_CROP = 20; // In pixels. Crops the border to reduce the black borders from stabilisation being too noticeable.

// 1. Get previous to current frame transformation (dx, dy, da) for all frames
// 2. Accumulate the transformations to get the image trajectory
// 3. Smooth out the trajectory using an averaging window
// 4. Generate new set of previous to current transform, such that the trajectory ends up being the same as the smoothed trajectory
// 5. Apply the new transformation to the video

struct TransformParam
{
    TransformParam() = default;
    TransformParam(double _dx, double _dy, double _da) {
        dx = _dx;
        dy = _dy;
        da = _da;
    }

    double dx;
    double dy;
    double da; // angle
};


const double pstd = 4e-3;//can be changed
const double cstd = 0.25;//can be changed

const Trajectory Q(pstd, pstd, pstd);// process noise covariance
const Trajectory R(cstd, cstd, cstd);// measurement noise covariance 



Stabilizer::Stabilizer()
// For further analysis
:out_transform("prev_to_cur_transformation.txt")
,out_trajectory("trajectory.txt")
,out_smoothed_trajectory("smoothed_trajectory.txt")
,out_new_transform("new_prev_to_cur_transformation.txt")
{
}

void Stabilizer::operator()(cv::Mat& cur)
{
    using std::vector;
    using cv::Point2f;

    if (k == 0)
    {
        k = 1;
        prev = cur;
        cvtColor(prev, prev_grey, cv::COLOR_BGR2GRAY);
        return;
    }


    const int vert_border = HORIZONTAL_BORDER_CROP * cur.rows / cur.cols; // get the aspect ratio correct

    cvtColor(cur, cur_grey, cv::COLOR_BGR2GRAY);

    // vector from prev to cur
    vector <Point2f> prev_corner;
    vector <Point2f> cur_corner;
    vector <Point2f> prev_corner2;
    vector <Point2f> cur_corner2;
    vector <uchar> status;
    vector <float> err;

    goodFeaturesToTrack(prev_grey, prev_corner, 200, 0.01, 30);
    calcOpticalFlowPyrLK(prev_grey, cur_grey, prev_corner, cur_corner, status, err);

    // weed out bad matches
    for (size_t i = 0; i < status.size(); i++) {
        if (status[i] != 0u) {
            prev_corner2.push_back(prev_corner[i]);
            cur_corner2.push_back(cur_corner[i]);
        }
    }

    // translation + rotation only
    auto T = estimateRigidTransform(prev_corner2, cur_corner2, false); // false = rigid transform, no scaling/shearing

    // in rare cases no transform is found. We'll just use the last known good transform.
    if (T.data == nullptr) {
        last_T.copyTo(T);
    }

    T.copyTo(last_T);

    // decompose T
    double dx = T.at<double>(0, 2);
    double dy = T.at<double>(1, 2);
    double da = atan2(T.at<double>(1, 0), T.at<double>(0, 0));

    out_transform << k << " " << dx << " " << dy << " " << da << '\n';
    //
    // Accumulated frame to frame transform
    x += dx;
    y += dy;
    a += da;
    //
    out_trajectory << k << " " << x << " " << y << " " << a << '\n';
    //
    Trajectory z{ x, y, a };
    //
    if (k == 1) {
        // intial guesses
        X = Trajectory(0, 0, 0); //Initial estimate,  set 0
        P = Trajectory(1, 1, 1); //set error variance,set 1
    }
    else
    {
        //time update（prediction）
        X_ = X; //X_(k) = X(k-1);
        P_ = P + Q; //P_(k) = P(k-1)+Q;
        // measurement update（correction）
        K = P_ / (P_ + R); //gain;K(k) = P_(k)/( P_(k)+R );
        X = X_ + K * (z - X_); //z-X_ is residual,X(k) = X_(k)+K(k)*(z(k)-X_(k)); 
        P = (Trajectory(1, 1, 1) - K)*P_; //P(k) = (1-K(k))*P_(k);
    }
    //smoothed_trajectory.push_back(X);
    out_smoothed_trajectory << k << " " << X.x << " " << X.y << " " << X.a << '\n';
    //-
    // target - current
    double diff_x = X.x - x;//
    double diff_y = X.y - y;
    double diff_a = X.a - a;

    dx = dx + diff_x;
    dy = dy + diff_y;
    da = da + diff_a;

    //
    out_new_transform << k << " " << dx << " " << dy << " " << da << '\n';
    //
    T.at<double>(0, 0) = cos(da);
    T.at<double>(0, 1) = -sin(da);
    T.at<double>(1, 0) = sin(da);
    T.at<double>(1, 1) = cos(da);

    T.at<double>(0, 2) = dx;
    T.at<double>(1, 2) = dy;

    cv::Mat cur2;

    warpAffine(prev, cur2, T, cur.size());

    cur2 = cur2(cv::Range(vert_border, cur2.rows - vert_border), 
        cv::Range(HORIZONTAL_BORDER_CROP, cur2.cols - HORIZONTAL_BORDER_CROP));

    // Resize cur2 back to cur size, for better side by side comparison
    resize(cur2, cur2, cur.size());

    prev = cur.clone();//cur.copyTo(prev);
    cur_grey.copyTo(prev_grey);

    cur = cur2;

    k++;
}
