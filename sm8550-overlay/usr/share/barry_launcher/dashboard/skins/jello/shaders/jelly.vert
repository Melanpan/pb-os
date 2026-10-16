#version 440
// The Jello skin's pads: the slab's picture on a fine grid, its points
// moved as thick jelly would move. Every tap sends a slow, heavy wobble
// out from where it landed: a ripple rolling outward and the whole slab
// squashing and stretching about that spot, the far side lagging behind,
// dying away over a few seconds. A finger held down presses a dent that
// trails it and oozes back when it lifts. Only drawing: the touches are
// Barry.Input's. Built with qsb (build-shaders.sh) into jelly.vert.qsb.

layout(location = 0) in vec4 qt_Vertex;
layout(location = 1) in vec2 qt_MultiTexCoord0;

layout(location = 0) out vec2 coord;
layout(location = 1) out float sheen;
layout(location = 2) out vec2 bend;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float now;    // seconds, JellyPad's clock
    float unit;   // the layout's scale (design pixels to item pixels)
    vec4 i0;      // the last four wobbles: x, y, start time, strength
    vec4 i1;
    vec4 i2;
    vec4 i3;
    vec4 dent;    // the finger: x, y, how deep (springs past 0 on letting go)
    vec2 size;    // the pad, item pixels
    float refraction;
};

vec2 wobble(vec4 im, vec2 p, inout float sh)
{
    float t = now - im.z;
    if (im.w <= 0.0 || t < 0.0 || t > 5.0)
        return vec2(0.0);
    vec2 d = p - im.xy;
    float r = length(d);
    vec2 dir = r > 0.5 ? d / r : vec2(0.0);
    float fade = exp(-t * 1.3) * im.w;

    // The ripple: slow, wide rings rolling out from the tap.
    float speed = 380.0 * unit;
    float front = 1.0 - smoothstep(speed * t - 90.0 * unit, speed * t + 90.0 * unit, r);
    float ring = sin(6.2831853 * r / (300.0 * unit) - 9.0 * t) * front * exp(-r / (600.0 * unit));
    vec2 move = dir * ring * 12.0 * unit * fade;

    // The slab: wider then taller about the tap, keeping its volume, the
    // jelly further away following late.
    float jig = sin(t * 7.0 - r / (420.0 * unit));
    move += vec2(d.x, -d.y) * 0.016 * jig * fade;

    sh += ring * fade;
    return move;
}

void main()
{
    vec2 p = qt_Vertex.xy;
    float sh = 0.0;
    vec2 move = wobble(i0, p, sh) + wobble(i1, p, sh) + wobble(i2, p, sh) + wobble(i3, p, sh);

    // The dent: the jelly around the finger drawn in toward it, deeper in
    // the middle; past 0 (oozing back) it bulges instead.
    vec2 toward = dent.xy - p;
    float near = exp(-dot(toward, toward) / (2.0 * pow(130.0 * unit, 2.0)));
    move += toward * 0.2 * dent.z * near;
    sh -= dent.z * near * 0.45;

    coord = qt_MultiTexCoord0;
    sheen = sh;
    bend = move / max(size, vec2(1.0));
    gl_Position = qt_Matrix * vec4(p + move, qt_Vertex.zw);
}
