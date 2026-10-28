#version 450
// Plain bilinear scaling of the game's picture into the (possibly letterboxed) area of the screen.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(texture(tex, uv).rgb, 1.0); }
