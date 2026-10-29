#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Extract {

    void setDllPath(const std::string& path);
    std::vector<std::string> shaderNames();

    ///
    /// Extract all known shaders.
    ///
    /// @throws std::runtime_error if shader extraction fails.
    ///
    void extractShaders();

    ///
    /// Get a shader by name.
    ///
    /// @param name The name of the shader to get.
    /// @return The shader bytecode.
    ///
    /// @throws std::runtime_error if the shader is not found.
    ///
    std::vector<uint8_t> getShader(const std::string& name);

}
