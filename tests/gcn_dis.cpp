// SPDX-License-Identifier: GPL-2.0-or-later
// gcn-dis FILE...: lists GCN programs (raw guest code, as BB_DUMP_SHADER_CODE writes them) with
// the recompiler's own decoder: opcode, operands, and the control fields that matter.
#include <cstdio>
#include <vector>
#include <magic_enum/magic_enum.hpp>
#include "shader_recompiler/frontend/decode.h"

using namespace Shader::Gcn;

static void PrintOperand(const InstOperand& op) {
    switch (op.field) {
    case OperandField::ScalarGPR: std::printf(" s%u", op.code); break;
    case OperandField::VectorGPR: std::printf(" v%u", op.code); break;
    case OperandField::LiteralConst: std::printf(" lit(%#x=%g)", op.code, std::bit_cast<float>(op.code)); break;
    case OperandField::SignedConstIntPos: std::printf(" %d", int(op.code) - 128); break;
    case OperandField::SignedConstIntNeg: std::printf(" %d", 192 - int(op.code)); break;
    case OperandField::Undefined: break;
    case OperandField::ConstZero: std::printf(" 0"); break;
    case OperandField::ConstFloatPos_0_5: std::printf(" 0.5"); break;
    case OperandField::ConstFloatNeg_0_5: std::printf(" -0.5"); break;
    case OperandField::ConstFloatPos_1_0: std::printf(" 1.0"); break;
    case OperandField::ConstFloatNeg_1_0: std::printf(" -1.0"); break;
    case OperandField::ConstFloatPos_2_0: std::printf(" 2.0"); break;
    case OperandField::ConstFloatNeg_2_0: std::printf(" -2.0"); break;
    case OperandField::ConstFloatPos_4_0: std::printf(" 4.0"); break;
    case OperandField::ConstFloatNeg_4_0: std::printf(" -4.0"); break;
    default: {
        const auto name = magic_enum::enum_name(op.field);
        if (name.empty()) {
            std::printf(" field%u", u32(op.field));
        } else {
            std::printf(" %s", std::string(name).c_str());
        }
        break;
    }
    }
    if (op.input_modifier.neg) std::printf("(neg)");
    if (op.input_modifier.abs) std::printf("(abs)");
}

int main(int argc, char** argv) {
    for (int a = 1; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb");
        if (!f) continue;
        std::vector<u32> code(1 << 16);
        code.resize(std::fread(code.data(), 4, code.size(), f));
        std::fclose(f);
        std::printf("== %s (%zu dwords)\n", argv[a], code.size());
        GcnCodeSlice slice(code.data(), code.data() + code.size());
        GcnDecodeContext decoder;
        u32 pc = 0;
        while (!slice.atEnd()) {
            const GcnInst inst = decoder.decodeInstruction(slice);
            std::printf("%04x %s", pc, std::string(magic_enum::enum_name(inst.opcode)).c_str());
            for (u32 i = 0; i < inst.dst_count; ++i) PrintOperand(inst.dst[i]);
            std::printf(",");
            for (u32 i = 0; i < inst.src_count; ++i) PrintOperand(inst.src[i]);
            switch (inst.category) {
            case InstCategory::VectorMemory:
                if (inst.inst_class == InstClass::VectorMemImgSmp || inst.inst_class == InstClass::VectorMemImgNoSmp)
                    std::printf("  dmask %u", u32(inst.control.mimg.dmask));
                else
                    std::printf("  off %u idxen %u offen %u", u32(inst.control.mubuf.offset), u32(inst.control.mubuf.idxen), u32(inst.control.mubuf.offen));
                break;
            case InstCategory::ScalarMemory:
                std::printf("  off %u imm %u", inst.control.smrd.offset, inst.control.smrd.imm);
                break;
            default: break;
            }
            if (inst.encoding == InstEncoding::VINTRP)
                std::printf("  attr %u chan %u", inst.control.vintrp.attr, inst.control.vintrp.chan);
            if (inst.encoding == InstEncoding::EXP)
                std::printf("  target %u en %#x", inst.control.exp.target, inst.control.exp.en);
            if (inst.encoding == InstEncoding::VOP3)
                std::printf("  omod %u clamp %u", inst.control.vop3.omod, inst.control.vop3.clmp);
            std::printf("\n");
            pc += inst.length;
            if (inst.opcode == Opcode::S_ENDPGM) break;
        }
    }
    return 0;
}
