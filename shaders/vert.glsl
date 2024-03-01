#version 460 core

out vec2 fs_texcoords;

layout(location = 0) uniform mat4 proj;
layout(location = 1) uniform vec2 tex_pos;
layout(location = 2) uniform vec2 tex_size;

uniform sampler2D tex;

void main()
{
	const vec2 pos_arr[4] = {{0.0, 0.0}, {0.0, 1.0}, {1.0, 0.0}, {1.0, 1.0}};
	vec2 pos = pos_arr[gl_VertexID];

	fs_texcoords = pos;

	gl_Position = proj * vec4(pos * tex_size + tex_pos, 0.0, 1.0);
}
