// based on https://ffmpeg.org/doxygen/trunk/remuxing_8c-example.html

#include "TransformVideo.h"
#include "Stabilizer.h"

#include <functional>


int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("You need to pass input and output file names as program parameters.\n");
        return EXIT_FAILURE;
    }
    
    const char *in_filename = argv[1];
    const char *out_filename = argv[2];
    
    try {
        Stabilizer stabilizer;
        return TransformVideo(in_filename, out_filename, std::ref(stabilizer));
    }
    catch (const std::exception& ex) {
        std::cerr << "Exception " << typeid(ex).name() << ": " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
