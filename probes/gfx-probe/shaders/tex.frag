#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColour;
layout(set = 0, binding = 0) uniform texture2D Tex;
layout(set = 0, binding = 1) uniform sampler Samp;
void main()
{
    outColour = texture(sampler2D(Tex, Samp), uv);
}
