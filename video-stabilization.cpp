// based on https://ffmpeg.org/doxygen/trunk/remuxing_8c-example.html

#include "TransformVideo.h"
#include "Stabilizer.h"

#include <functional>


int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("You need to pass at least two parameters.\n");
        return -1;
    }
    
    const char *in_filename = argv[1];
    const char *out_filename = argv[2];
    

    Stabilizer stabilizer;

    return TransformVideo(in_filename, out_filename, std::ref(stabilizer));
}
