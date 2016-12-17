// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "video_core/shader/shader.h"

namespace Pica {

namespace Shader {

class InterpreterEngine final : public ShaderEngine {
public:
    void SetupBatch(const ShaderSetup* setup) override;
    void Run(UnitState& state, unsigned int entry_point) const override;
    DebugData<true> ProduceDebugInfo(const InputVertex& input, int num_attributes,
                                     unsigned int entry_point) const override;

private:
    const ShaderSetup* setup = nullptr;
};

} // namespace

} // namespace
