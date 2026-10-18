// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

// The frontend overlay: vertices in window pixels, mapped to clip space.
layout (location = 0) in vec2 position;
layout (location = 1) in vec2 tex_coord;
layout (location = 2) in vec4 color;

layout (push_constant) uniform PushConstants {
    vec2 scale;
    vec2 translate;
};

layout (location = 0) out vec4 frag_color;
layout (location = 1) out vec2 frag_tex_coord;

void main() {
    frag_color = color;
    frag_tex_coord = tex_coord;
    gl_Position = vec4(position * scale + translate, 0.0, 1.0);
}
