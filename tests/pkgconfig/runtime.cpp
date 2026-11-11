// Reads the image writer.cpp wrote (argv[1]) and randomizes it with the
// static runtime, checking 10 < x < 20 on every seed.
//
// It also instantiates templates the runtime's C++ code (CaDiCaL) does. This
// object comes before the archive on the link line, so its COMDAT copies win
// and the runtime's are discarded: the runtime must then reach ours by name.
#include <dv_solve/dv_solve.h>

#include <cstdint>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<unsigned char> buf;
    for (int c; (c = std::fgetc(f)) != EOF;) buf.push_back(static_cast<unsigned char>(c));
    std::fclose(f);

    std::vector<int> ints;
    std::vector<unsigned> uints;
    for (int i = 0; i < 4; ++i) {
        ints.push_back(i);
        const unsigned u = static_cast<unsigned>(i);
        uints.push_back(u);
    }
    if (ints.size() != 4 || uints.size() != 4) return 1;

    dvs_handle_t* h = dvs_handle_new(buf.data(), buf.size());
    if (!h) {
        std::printf("FAIL dvs_handle_new\n");
        return 1;
    }
    uint8_t x = 0;
    dvs_ref_t ref = {&x, 8, 0, DVS_REF_SCALAR, 0};
    int bad = 0;
    for (uint64_t seed = 1; seed <= 8; ++seed) {
        int rc = dvs_randomize(h, &ref, nullptr, seed);
        std::printf("seed %llu: rc %d x %u\n", static_cast<unsigned long long>(seed), rc, x);
        if (rc != DVS_RANDOMIZE_OK || x <= 10 || x >= 20) ++bad;
    }
    dvs_handle_free(h);
    std::printf("%s (runtime format version %u)\n", bad ? "FAIL" : "PASS",
                dvs_problem_format_version());
    return bad ? 1 : 0;
}
