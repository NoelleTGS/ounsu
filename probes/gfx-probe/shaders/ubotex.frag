#version 450
// osu-framework's layout: set 0 = global uniform buffer, set 1 = texture + sampler.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColour;
layout(set = 0, binding = 0) uniform Globals { vec4 Tint; };
layout(set = 1, binding = 0) uniform texture2D Tex;
layout(set = 1, binding = 1) uniform sampler Samp;
void main()
{
    outColour = texture(sampler2D(Tex, Samp), uv) * Tint;
}
