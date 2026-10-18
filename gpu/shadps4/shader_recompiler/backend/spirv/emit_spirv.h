// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Backend::SPIRV {

// A loop-free GS cannot emit more vertices than its static emission sites.
[[nodiscard]] u32 GeometryOutputVertexLimit(const IR::Program& program, u32 declared);

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info,
                                         const IR::Program& program, Bindings& binding);

} // namespace Shader::Backend::SPIRV
