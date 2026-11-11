// Builds an EXPLICIT problem -- 8-bit RAND x, x > 10 && x < 20 -- with the
// writer library alone, validates it, and writes its image to argv[1].
#include <dv_solve/dvs_writer.h>

#include <cstdint>
#include <cstdio>

static dvs_expr_t const8(dvs_builder_t* b, int64_t v) {
    return dvs_builder_expr_const_sized(b, v, 0, 8);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    dvs_builder_t* b = dvs_builder_create(0, nullptr);
    dvs_builder_set_explicit(b);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_expr_t x = dvs_builder_expr_var(b, 0);
    dvs_builder_add_constraint(
        b, dvs_builder_expr_binary(b, DVS_BIN_AND,
                                   dvs_builder_expr_binary(b, DVS_BIN_GT, x, const8(b, 10)),
                                   dvs_builder_expr_binary(b, DVS_BIN_LT, x, const8(b, 20))));
    size_t size = 0;
    dvs_problem_t* p = dvs_builder_finalize(b, &size);
    char why[200] = "";
    if (!p || dvs_problem_validate(p, why, sizeof why) != 0) {
        std::printf("FAIL validate: %s\n", why);
        return 1;
    }
    FILE* f = std::fopen(argv[1], "wb");
    if (!p || !f || std::fwrite(p, 1, size, f) != size) return 1;
    std::fclose(f);
    dvs_builder_free_problem(b, p, size);
    dvs_builder_destroy(b);
    std::printf("wrote %zu bytes, format version %u\n", size, dvs_problem_format_version());
    return 0;
}
