#version 440
// The Jello skin's pads: the see-through slab (jelly.vert moves it) over
// what it sits on, the plate, which shows through bent the way the jelly
// is bent; lighter where a ripple crests, darker in a dent. Built with
// qsb (build-shaders.sh) into jelly.frag.qsb.

layout(location = 0) in vec2 coord;
layout(location = 1) in float sheen;
layout(location = 2) in vec2 bend;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float now;
    float unit;
    vec4 i0;
    vec4 i1;
    vec4 i2;
    vec4 i3;
    vec4 dent;
    vec2 size;
    float refraction;
};

layout(binding = 1) uniform sampler2D source;  // the slab, premultiplied
layout(binding = 2) uniform sampler2D behind;  // the plate under it

void main()
{
    vec4 j = texture(source, coord);
    float light = clamp(sheen, -1.0, 1.0);
    j.rgb = light > 0.0 ? mix(j.rgb, vec3(j.a), light * 0.22) : j.rgb * (1.0 + light * 0.28);
    // The plate seen through the jelly, bent against its movement.
    vec4 b = texture(behind, coord - bend * refraction + vec2(light) * 0.004);
    fragColor = (j + b * (1.0 - j.a)) * qt_Opacity;
}
