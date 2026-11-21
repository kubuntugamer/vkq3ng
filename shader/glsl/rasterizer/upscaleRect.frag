#version 450

/*
 * Edge-adaptive spatial upscale (FSR1 EASU-style), used when the RTX scene
 * is rendered below the swapchain resolution.  The 2D/UI path keeps using
 * the plain fullscreenRect shader, so text and HUD stay untouched.
 */

layout(location = 0) in vec2 v_uv;

layout(binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Push {
    vec4 src; // xy = active source size, zw = full image size
} pc;

layout(location = 0) out vec4 fragColor;

float luma(vec3 c)
{
    return 0.5 * c.r + c.g + 0.5 * c.b;
}

void easuSet(inout vec2 dir, inout float len, vec2 pp, float weight,
             float lA, float lB, float lC, float lD, float lE)
{
    float dc = lD - lC;
    float cb = lC - lB;
    float lengthX = max(abs(dc), abs(cb));
    lengthX = 1.0 / max(lengthX, 1.0e-8);
    float directionX = lD - lB;
    dir.x += directionX * weight;
    lengthX = clamp(abs(directionX) * lengthX, 0.0, 1.0);
    lengthX *= lengthX;
    len += lengthX * weight;

    float ec = lE - lC;
    float ca = lC - lA;
    float lengthY = max(abs(ec), abs(ca));
    lengthY = 1.0 / max(lengthY, 1.0e-8);
    float directionY = lE - lA;
    dir.y += directionY * weight;
    lengthY = clamp(abs(directionY) * lengthY, 0.0, 1.0);
    lengthY *= lengthY;
    len += lengthY * weight;
}

void easuTap(inout vec3 accumulatedColor, inout float accumulatedWeight,
             vec2 pixelOffset, vec2 dir, vec2 len2, float lob, float clp,
             vec3 color)
{
    vec2 rotatedOffset;
    rotatedOffset.x = pixelOffset.x * dir.x + pixelOffset.y * dir.y;
    rotatedOffset.y = pixelOffset.x * (-dir.y) + pixelOffset.y * dir.x;
    rotatedOffset *= len2;

    float distanceSquared = dot(rotatedOffset, rotatedOffset);
    distanceSquared = min(distanceSquared, clp);

    float weightB = (2.0 / 5.0) * distanceSquared - 1.0;
    float weightA = lob * distanceSquared - 1.0;
    weightB *= weightB;
    weightA *= weightA;
    weightB = (25.0 / 16.0) * weightB - (25.0 / 16.0 - 1.0);
    float weight = weightB * weightA;

    accumulatedColor += color * weight;
    accumulatedWeight += weight;
}

void main()
{
    vec2 fullSize = vec2(textureSize(tex, 0));
    vec2 activeSize = pc.src.xy;
    vec2 invFullSize = 1.0 / fullSize;

    // Source position of this output fragment, relative to pixel 'f'.
    vec2 src = v_uv * activeSize - 0.5;
    vec2 fp = floor(src);
    vec2 pp = src - fp;

    // The active render region lives in the top-left of a full-size image.
    #define SAMPLE_TEXEL(p) textureLod(tex, (clamp((p), vec2(0.0), activeSize - vec2(1.0)) + 0.5) * invFullSize, 0.0)

    // 12-tap kernel around f:
    //     b c
    //   e f g h
    //   i j k l
    //     n o
    vec3 b = SAMPLE_TEXEL(fp + vec2( 0.0, -1.0)).rgb;
    vec3 c = SAMPLE_TEXEL(fp + vec2( 1.0, -1.0)).rgb;
    vec3 e = SAMPLE_TEXEL(fp + vec2(-1.0,  0.0)).rgb;
    vec3 f = SAMPLE_TEXEL(fp + vec2( 0.0,  0.0)).rgb;
    vec3 g = SAMPLE_TEXEL(fp + vec2( 1.0,  0.0)).rgb;
    vec3 h = SAMPLE_TEXEL(fp + vec2( 2.0,  0.0)).rgb;
    vec3 i = SAMPLE_TEXEL(fp + vec2(-1.0,  1.0)).rgb;
    vec3 j = SAMPLE_TEXEL(fp + vec2( 0.0,  1.0)).rgb;
    vec3 k = SAMPLE_TEXEL(fp + vec2( 1.0,  1.0)).rgb;
    vec3 l = SAMPLE_TEXEL(fp + vec2( 2.0,  1.0)).rgb;
    vec3 n = SAMPLE_TEXEL(fp + vec2( 0.0,  2.0)).rgb;
    vec3 o = SAMPLE_TEXEL(fp + vec2( 1.0,  2.0)).rgb;

    float bL = luma(b);
    float cL = luma(c);
    float eL = luma(e);
    float fL = luma(f);
    float gL = luma(g);
    float hL = luma(h);
    float iL = luma(i);
    float jL = luma(j);
    float kL = luma(k);
    float lL = luma(l);
    float nL = luma(n);
    float oL = luma(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    float w0 = (1.0 - pp.x) * (1.0 - pp.y);
    float w1 = pp.x * (1.0 - pp.y);
    float w2 = (1.0 - pp.x) * pp.y;
    float w3 = pp.x * pp.y;
    easuSet(dir, len, pp, w0, bL, eL, fL, gL, jL);
    easuSet(dir, len, pp, w1, cL, fL, gL, hL, kL);
    easuSet(dir, len, pp, w2, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp, w3, gL, jL, kL, lL, oL);

    float dirR = dot(dir, dir);
    bool zero = dirR < (1.0 / 32768.0);
    dirR = zero ? 1.0 : inversesqrt(dirR);
    dir = zero ? vec2(1.0) : dir;
    dir *= dirR;

    len *= 0.5;
    len *= len;

    float stretch = dot(dir, dir) * (1.0 / max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));

    vec3 accumulatedColor = vec3(0.0);
    float accumulatedWeight = 0.0;
    easuTap(accumulatedColor, accumulatedWeight, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(accumulatedColor, accumulatedWeight, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    easuTap(accumulatedColor, accumulatedWeight, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
    easuTap(accumulatedColor, accumulatedWeight, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);

    vec3 pix = accumulatedColor / max(accumulatedWeight, 1.0e-8);
    pix = clamp(pix, min4, max4);

    fragColor = vec4(pix, 1.0);
}
