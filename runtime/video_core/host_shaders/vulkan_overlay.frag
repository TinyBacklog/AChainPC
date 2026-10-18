// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (binding = 0) uniform sampler2D font;

layout (location = 0) in vec4 frag_color;
layout (location = 1) in vec2 frag_tex_coord;
layout (location = 0) out vec4 color;

void main() {
    color = frag_color * texture(font, frag_tex_coord);
}
