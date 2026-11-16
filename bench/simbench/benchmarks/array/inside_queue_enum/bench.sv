// array.inside_queue_enum: riscv-dv's riscv_instr::get_rand_instr, an enum
// `inside` a queue of allowed values and `!inside` a queue of excluded ones
// (std::randomize() with, on a function's queue arguments). The data is that of
// riscv_rand_jump_test seed 1 (rv32imc): 87 instruction names over a 506-value
// enum, an include list of 61, an exclude list of 4, so 60 solutions.
// See docs/design/dvs_hotspot_h1.md in the Verilator tree.
typedef enum {
    I0, I1, I2, I3, I4, I5, I6, I7, I8, I9, I10, I11, I12, I13, I14, I15, I16, I17, I18, I19,
    I20, I21, I22, I23, I24, I25, I26, I27, I28, I29, I30, I31, I32, I33, I34, I35, I36, I37,
    I38, I39, I40, I41, I42, I43, I44, I45, I46, I47, I48, I49, I50, I51, I52, I53, I54, I55,
    I56, I57, I58, I59, I60, I61, I62, I63, I64, I65, I66, I67, I68, I69, I70, I71, I72, I73,
    I74, I75, I76, I77, I78, I79, I80, I81, I82, I83, I84, I85, I86, I87, I88, I89, I90, I91,
    I92, I93, I94, I95, I96, I97, I98, I99, I100, I101, I102, I103, I104, I105, I106, I107,
    I108, I109, I110, I111, I112, I113, I114, I115, I116, I117, I118, I119, I120, I121, I122,
    I123, I124, I125, I126, I127, I128, I129, I130, I131, I132, I133, I134, I135, I136, I137,
    I138, I139, I140, I141, I142, I143, I144, I145, I146, I147, I148, I149, I150, I151, I152,
    I153, I154, I155, I156, I157, I158, I159, I160, I161, I162, I163, I164, I165, I166, I167,
    I168, I169, I170, I171, I172, I173, I174, I175, I176, I177, I178, I179, I180, I181, I182,
    I183, I184, I185, I186, I187, I188, I189, I190, I191, I192, I193, I194, I195, I196, I197,
    I198, I199, I200, I201, I202, I203, I204, I205, I206, I207, I208, I209, I210, I211, I212,
    I213, I214, I215, I216, I217, I218, I219, I220, I221, I222, I223, I224, I225, I226, I227,
    I228, I229, I230, I231, I232, I233, I234, I235, I236, I237, I238, I239, I240, I241, I242,
    I243, I244, I245, I246, I247, I248, I249, I250, I251, I252, I253, I254, I255, I256, I257,
    I258, I259, I260, I261, I262, I263, I264, I265, I266, I267, I268, I269, I270, I271, I272,
    I273, I274, I275, I276, I277, I278, I279, I280, I281, I282, I283, I284, I285, I286, I287,
    I288, I289, I290, I291, I292, I293, I294, I295, I296, I297, I298, I299, I300, I301, I302,
    I303, I304, I305, I306, I307, I308, I309, I310, I311, I312, I313, I314, I315, I316, I317,
    I318, I319, I320, I321, I322, I323, I324, I325, I326, I327, I328, I329, I330, I331, I332,
    I333, I334, I335, I336, I337, I338, I339, I340, I341, I342, I343, I344, I345, I346, I347,
    I348, I349, I350, I351, I352, I353, I354, I355, I356, I357, I358, I359, I360, I361, I362,
    I363, I364, I365, I366, I367, I368, I369, I370, I371, I372, I373, I374, I375, I376, I377,
    I378, I379, I380, I381, I382, I383, I384, I385, I386, I387, I388, I389, I390, I391, I392,
    I393, I394, I395, I396, I397, I398, I399, I400, I401, I402, I403, I404, I405, I406, I407,
    I408, I409, I410, I411, I412, I413, I414, I415, I416, I417, I418, I419, I420, I421, I422,
    I423, I424, I425, I426, I427, I428, I429, I430, I431, I432, I433, I434, I435, I436, I437,
    I438, I439, I440, I441, I442, I443, I444, I445, I446, I447, I448, I449, I450, I451, I452,
    I453, I454, I455, I456, I457, I458, I459, I460, I461, I462, I463, I464, I465, I466, I467,
    I468, I469, I470, I471, I472, I473, I474, I475, I476, I477, I478, I479, I480, I481, I482,
    I483, I484, I485, I486, I487, I488, I489, I490, I491, I492, I493, I494, I495, I496, I497,
    I498, I499, I500, I501, I502, I503, I504, I505
} sb_name_t;

class sb_item;
  static sb_name_t instr_names[$] = {
      I0, I1, I2, I3, I4, I5, I6, I7, I8, I9, I10, I11, I12, I13, I14, I15, I16, I17, I18,
      I19, I20, I21, I22, I23, I24, I25, I26, I27, I28, I29, I30, I31, I32, I33, I34, I35,
      I36, I37, I38, I39, I40, I41, I42, I43, I44, I45, I46, I47, I159, I160, I161, I162,
      I163, I164, I165, I166, I246, I247, I248, I249, I250, I251, I252, I254, I255, I256,
      I257, I258, I259, I260, I261, I262, I263, I264, I265, I266, I267, I268, I269, I270,
      I271, I272, I501, I502, I503, I504, I505};
  static sb_name_t include_instr[$] = {
      I24, I25, I26, I29, I33, I34, I255, I256, I264, I0, I1, I18, I27, I28, I37, I159, I160,
      I161, I162, I163, I164, I165, I166, I250, I251, I252, I254, I258, I265, I267, I268,
      I21, I22, I23, I32, I35, I36, I257, I259, I260, I261, I19, I20, I30, I31, I38, I39,
      I42, I43, I44, I45, I46, I47, I4, I5, I6, I7, I8, I9, I262, I263};
  static sb_name_t exclude_instr[$] = {I250, I253, I248, I278};
  sb_name_t name;

  // As riscv_instr::get_rand_instr: the queues are function arguments
  static function bit pick(output sb_name_t r, input sb_name_t include_instr[$],
                           input sb_name_t allowed_instr[$], input sb_name_t disallowed_instr[$]);
    sb_name_t name;
    int ok = std::randomize(name) with {
      name inside {instr_names};
      if (include_instr.size() > 0) {
        name inside {include_instr};
      }
      if (allowed_instr.size() > 0) {
        name inside {allowed_instr};
      }
      if (disallowed_instr.size() > 0) {
        !(name inside {disallowed_instr});
      }
    };
    r = name;
    return ok != 0;
  endfunction

  function bit ok();
    return name inside {instr_names} && name inside {include_instr}
           && !(name inside {exclude_instr});
  endfunction
endclass

class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  sb_name_t empty[$];
  function new(); super.new("array.inside_queue_enum"); endfunction
  virtual function bit do_randomize();
    return sb_item::pick(it.name, sb_item::include_instr, empty, sb_item::exclude_instr);
  endfunction
  virtual function int check();
    return it.ok() ? 0 : 1;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d\n", it.name);
  endfunction
endclass
