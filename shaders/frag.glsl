#version 460 core
in vec2 fs_texcoords;

out vec4 frag_color;

uniform sampler2D tex;

void main()
{
    frag_color = texture(tex, fs_texcoords);
}
