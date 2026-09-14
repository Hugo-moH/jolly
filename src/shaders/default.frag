#version 450

layout(set = 0, binding = 0) uniform sampler2D source;

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(texture(source, texCoord).rgb, 1.0);
}
