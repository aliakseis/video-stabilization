#pragma once

#include <functional>

namespace cv {
    
class Mat;

}

int TransformVideo(const char *in_filename, const char *out_filename, std::function<void(cv::Mat&)>  callback);
