// SPDX-License-Identifier: GPL-2.0-or-later
// A pipeline loaded from the shader cache keeps its vertex shader's fetch shader whatever order
// its stages load in. Stages load in SwStage order, so the geometry stage of a VS+GS pipeline
// came after the vertex stage and reset it (LBP3: a cached pipeline drew with no vertex inputs,
// VUID-vkCmdDrawIndexed-Input-07939).
#include <cassert>
#include <cstdio>

#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace {
std::vector<u8> Meta(Shader::SwStage sw, Shader::HwStage hw, bool with_fetch) {
    const std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::Info info{hw, sw, Shader::ShaderParams{.user_data = user_data, .code = {},
                                                         .hash = u64(sw) + 1}};
    Shader::StageSpecialization spec{};
    if (with_fetch) {
        Shader::Gcn::FetchShaderData fetch{};
        fetch.attributes.push_back({.semantic = 0, .dest_vgpr = 4, .num_elements = 4});
        fetch.vertex_offset_sgpr = 2;
        spec.fetch_shader_data = fetch;
    }
    Serialization::Archive ar;
    Vulkan::WriteShaderMeta(ar, info, spec, u64(sw) + 1, 0);
    return std::move(ar.TakeOff());
}

std::optional<Shader::Gcn::FetchShaderData> Load(std::initializer_list<std::vector<u8>> stages) {
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    for (const auto& blob : stages) {
        Serialization::Archive ar{std::vector<u8>{blob}};
        Shader::Info info{};
        Shader::StageSpecialization spec{};
        spec.info = &info;
        size_t perm_idx = ~size_t{0};
        const bool loaded = Vulkan::LoadShaderMeta(ar, info, fetch, spec, perm_idx);
        assert(loaded && perm_idx == 0);
    }
    return fetch;
}
} // namespace

int main() {
    // A GS with four static emissions and a loose hardware bound of 64 needs only four
    // vertices of output storage. Conditional blocks still provide a conservative bound.
    Shader::Info info{};
    Shader::IR::Program program{info};
    Common::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block first{pool}, second{pool};
    for (int i = 0; i < 2; ++i) {
        first.AppendNewInst(Shader::IR::Opcode::EmitVertex, {});
        second.AppendNewInst(Shader::IR::Opcode::EmitVertex, {});
    }
    using Node = Shader::IR::AbstractSyntaxNode;
    program.syntax_list.push_back({.data = {.block = &first}, .type = Node::Type::Block});
    program.syntax_list.push_back({.type = Node::Type::If});
    program.syntax_list.push_back({.data = {.block = &second}, .type = Node::Type::Block});
    program.syntax_list.push_back({.type = Node::Type::EndIf});
    using Shader::Backend::SPIRV::GeometryOutputVertexLimit;
    assert(GeometryOutputVertexLimit(program, 64) == 4);
    assert(GeometryOutputVertexLimit(program, 3) == 3);
    program.syntax_list.push_back({.type = Node::Type::Loop});
    assert(GeometryOutputVertexLimit(program, 64) == 64);
    program.syntax_list.back().type = Node::Type::Repeat;
    assert(GeometryOutputVertexLimit(program, 64) == 64);
    program.syntax_list.clear();
    assert(GeometryOutputVertexLimit(program, 64) == 64);

    const auto vs = Meta(Shader::SwStage::Vertex, Shader::HwStage::Export, true);
    const auto gs = Meta(Shader::SwStage::Geometry, Shader::HwStage::Geometry, false);
    const auto fs = Meta(Shader::SwStage::Fragment, Shader::HwStage::Fragment, false);

    const auto vs_gs = Load({fs, vs, gs}); // the cache's order
    assert(vs_gs && vs_gs->attributes.size() == 1 && vs_gs->vertex_offset_sgpr == 2);
    const auto gs_first = Load({gs, vs});
    assert(gs_first && gs_first->attributes.size() == 1);
    assert(!Load({fs, gs}));
    std::printf("Shaders: bounded loop-free GS emissions; cached VS+GS keeps its fetch shader\n");
    return 0;
}
