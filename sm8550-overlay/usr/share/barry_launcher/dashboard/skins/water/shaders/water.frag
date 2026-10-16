#version 440
// The Water skin's pads: the pool (WaterPad.qml's picture of it) seen
// through a water surface. Every drop (a finger landing, a finger moving,
// a click) sends rings out from where it fell: a short train of waves that
// spreads, slows into longer waves and dies away over a few seconds. The
// rings bend the pool under them and catch the light on their crests, and
// the floor's caustics bend with them. Only drawing: the touches are
// Barry.Input's. Built with qsb (build-shaders.sh) into water.frag.qsb.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float now;      // seconds, WaterPad's clock
    float unit;     // the layout's scale (design pixels to item pixels)
    float caustics; // how bright the floor's light pattern is
    vec2 size;      // the pad, item pixels
    // The last 24 drops: x, y, when it fell, strength (0: none).
    vec4 r0; vec4 r1; vec4 r2; vec4 r3; vec4 r4; vec4 r5;
    vec4 r6; vec4 r7; vec4 r8; vec4 r9; vec4 r10; vec4 r11;
    vec4 r12; vec4 r13; vec4 r14; vec4 r15; vec4 r16; vec4 r17;
    vec4 r18; vec4 r19; vec4 r20; vec4 r21; vec4 r22; vec4 r23;
};

layout(binding = 1) uniform sampler2D source;  // the pool, premultiplied

const float LIFE = 3.0;
const float TAU = 6.2831853;

// The slope of one drop's rings at p (the surface's height falls off
// along it), and its height into h.
vec2 ring(vec4 d, vec2 p, inout float h)
{
    float t = now - d.z;
    if (d.w <= 0.0 || t < 0.0 || t > LIFE)
        return vec2(0.0);
    vec2 off = p - d.xy;
    float r = length(off);
    vec2 dir = r > 0.5 ? off / r : vec2(0.0);

    // A train of a few waves just behind the front, which runs out at a
    // steady speed while the train widens and its waves grow longer.
    float u = r - 330.0 * unit * t;
    float w = (22.0 + 46.0 * t) * unit;
    float k = TAU / ((44.0 + 34.0 * t) * unit);
    float c = u + 0.9 * w;
    float env = exp(-c * c / (2.0 * w * w));
    float amp = d.w * exp(-t * 1.6) / sqrt(1.0 + r / (90.0 * unit));

    h += amp * sin(k * u) * env;
    return dir * amp * env * (k * cos(k * u) - c / (w * w) * sin(k * u));
}

// A point in each cell of a grid, drifting slowly about its middle.
vec2 cellPoint(vec2 cell, float t)
{
    vec2 h = fract(sin(vec2(dot(cell, vec2(127.1, 311.7)), dot(cell, vec2(269.5, 183.3)))) * 43758.5453);
    return cell + 0.5 + 0.38 * sin(t + TAU * h);
}

// How near q is to the line between two cells' points: 1 on it, 0 away.
float cellEdges(vec2 q, float t)
{
    vec2 base = floor(q);
    float d1 = 9.0, d2 = 9.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float d = length(q - cellPoint(base + vec2(x, y), t));
            if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) { d2 = d; }
        }
    }
    return 1.0 - smoothstep(0.0, 0.3, d2 - d1);
}

// Light on a pool floor through still water: a net of wavy bright lines,
// two layers of it, brightest where they cross.
float floorLight(vec2 q, float t)
{
    q += 0.18 * sin(q.yx * 1.9 + vec2(1.3, 0.4));
    float a = cellEdges(q, t);
    float b = cellEdges(q * 1.43 + vec2(5.2, 1.7), t * 1.3 + 2.0);
    return clamp(pow(a, 2.5) * 0.65 + pow(b, 3.0) * 0.45 + a * b * 0.5, 0.0, 1.0);
}

void main()
{
    vec2 p = qt_TexCoord0 * size;
    vec4 drops[24] = vec4[24](r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                              r12, r13, r14, r15, r16, r17, r18, r19, r20, r21, r22, r23);
    float h = 0.0;
    vec2 slope = vec2(0.0);
    for (int n = 0; n < 24; n++)
        slope += ring(drops[n], p, h);

    // Still near the rim, so the pool's edge stays put.
    float edge = min(min(p.x, p.y), min(size.x - p.x, size.y - p.y));
    float keep = smoothstep(0.0, 46.0 * unit, edge);
    slope *= keep;
    h *= keep;

    // The pool bent by the surface, as through water.
    vec2 bent = p + slope * 130.0 * unit;
    vec4 pool = texture(source, bent / size);
    float inside = texture(source, qt_TexCoord0).a;

    // The surface's light: brighter on the slopes facing the light (up and
    // to the left), a glint on the sharpest crests.
    vec3 n = normalize(vec3(-slope * 5.0, 1.0));
    vec3 toLight = normalize(vec3(-0.45, -0.6, 1.0));
    float shade = dot(n, toLight) - toLight.z;
    float glint = pow(max(dot(reflect(-toLight, n), vec3(0.0, 0.0, 1.0)), 0.0), 60.0) * smoothstep(0.02, 0.12, length(slope));

    // The floor's net of light, bent with the pool and drifting a little
    // while the water moves.
    float net = floorLight(bent / (240.0 * unit), now * 0.25) * caustics;

    vec3 rgb = pool.rgb + (vec3(0.75, 0.95, 1.0) * net * 0.24 + vec3(0.8, 0.95, 1.0) * shade * 0.9 + vec3(glint) * 0.7 + vec3(h) * 0.25) * pool.a;
    rgb = clamp(rgb, 0.0, 1.0) * inside;
    fragColor = vec4(min(rgb, vec3(pool.a * inside)), pool.a * inside) * qt_Opacity;
}
