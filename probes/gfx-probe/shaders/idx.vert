#version 450
// Indexed quad like osu-framework's batches: position + depth per vertex, uv from position.
layout(location = 0) in vec2 Position;
layout(location = 1) in float Depth;
layout(location = 0) out vec2 uv;
void main()
{
    uv = Position * 0.5 + 0.5;
    gl_Position = vec4(Position, Depth, 1.0);
}
